#include <stack>

#include <cudf/table/table_view.hpp>
#include <cudf/join.hpp>
#include <cudf/stream_compaction.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/copying.hpp>
#include <cudf/partitioning.hpp>

#include "LogicalFilter.h"
#include "LogicalProject.h"
#include "../../CalciteExpressionParsing.h"
#include <blazingdb/io/Library/Logging/Logger.h>

#include "distribution/primitives.h"
#include "communication/CommunicationData.h"
#include "utilities/CommonOperations.h"
#include "utilities/DebuggingUtils.h"
#include "utilities/StringUtils.h"
#include "Utils.cuh"

namespace ral {
namespace processor {
namespace {

const std::string LOGICAL_FILTER = "LogicalFilter";

const std::string INNER_JOIN = "inner";
const std::string LEFT_JOIN = "left";
const std::string RIGHT_JOIN = "right";
const std::string OUTER_JOIN = "full";

} // namespace

bool is_logical_filter(const std::string & query_part) {
  return query_part.find(LOGICAL_FILTER) != std::string::npos;
}

std::unique_ptr<ral::frame::BlazingTable> applyBooleanFilter(
  const ral::frame::BlazingTableView & table,
  const CudfColumnView & boolValues){
  auto filteredTable = cudf::experimental::apply_boolean_mask(
    table.view(),boolValues);
  return std::make_unique<ral::frame::BlazingTable>(std::move(
    filteredTable),table.names());
}

std::unique_ptr<ral::frame::BlazingTable> process_filter(
  const ral::frame::BlazingTableView & table_view,
  const std::string & query_part,
  blazingdb::manager::experimental::Context * context) {

	if(table_view.num_rows() == 0) {
		return std::make_unique<ral::frame::BlazingTable>(cudf::experimental::empty_like(table_view.view()), table_view.names());
	}
	
  std::string conditional_expression = get_named_expression(query_part, "condition");
	if(conditional_expression.empty()) {
		conditional_expression = get_named_expression(query_part, "filters");
	}

  std::vector<std::unique_ptr<ral::frame::BlazingColumn>> evaluated_table = evaluate_expressions(table_view.toBlazingColumns(), {conditional_expression});

  RAL_EXPECTS(evaluated_table.size() == 1 && evaluated_table[0]->view().type().id() == cudf::type_id::BOOL8, "Expression does not evaluate to a boolean mask");

  return applyBooleanFilter(table_view, evaluated_table[0]->view());
}


  namespace{
    typedef std::pair<blazingdb::transport::experimental::Node, std::unique_ptr<ral::frame::BlazingTable> > NodeColumn;
    typedef std::pair<blazingdb::transport::experimental::Node, ral::frame::BlazingTableView > NodeColumnView;
  }

  void parseJoinConditionToColumnIndices(const std::string & condition, std::vector<int> & columnIndices) {
	// TODO: right now this only works for equijoins
	// since this is all that is implemented at the time

	// TODO: for this to work properly we can only do multi column join
	// when we have ands, when we have hors we hvae to perform the joisn seperately then
	// do a unique merge of the indices

	// right now with pred push down the join codnition takes the filters as the second argument to condition

	std::string clean_expression = clean_calcite_expression(condition);
	int operator_count = 0;
	std::stack<std::string> operand;
	// NOTE percy c.cordoba here we dont need to call fix_tokens_after_call_get_tokens_in_reverse_order_for_timestamp
	// after
	std::vector<std::string> tokens = get_tokens_in_reverse_order(clean_expression);
	for(std::string token : tokens) {
		if(is_operator_token(token)) {
			if(token == "=") {
				// so far only equijoins are supported in libgdf
				operator_count++;
			} else if(token != "AND") {
				throw std::runtime_error("In evaluate_join function: unsupported non-equijoins operator");
			}
		} else {
			operand.push(token);
		}
	}

	columnIndices.resize(2 * operator_count);
	for(size_t i = 0; i < operator_count; i++) {
		int right_index = get_index(operand.top());
		operand.pop();
		int left_index = get_index(operand.top());
		operand.pop();

		if(right_index < left_index) {
			std::swap(left_index, right_index);
		}

		columnIndices[2 * i] = left_index;
		columnIndices[2 * i + 1] = right_index;
	}
}

  std::unique_ptr<ral::frame::BlazingTable> process_distribution_table(
  	const ral::frame::BlazingTableView & table,
    std::vector<int> & columnIndices,
    blazingdb::manager::experimental::Context * context) {

    std::vector<NodeColumnView > partitions;
    std::unique_ptr<CudfTable> hashed_data;
    if (table.num_rows() > 0){    
      //TODO: CACHE_POINT (ask felipe)
      std::vector<cudf::size_type> columns_to_hash;
      std::transform(columnIndices.begin(), columnIndices.end(), std::back_inserter(columns_to_hash), [](int index) { return (cudf::size_type)index; });
      
      std::vector<cudf::size_type> hased_data_offsets;
      std::tie(hashed_data, hased_data_offsets) = cudf::experimental::hash_partition(table.view(),
              columns_to_hash, context->getTotalNodes());

      // the offsets returned by hash_partition will always start at 0, which is a value we want to ignore for cudf::split
      std::vector<cudf::size_type> split_indexes(hased_data_offsets.begin() + 1, hased_data_offsets.end());
      std::vector<CudfTableView> partitioned = cudf::experimental::split(hashed_data->view(), split_indexes);
      
      for(int nodeIndex = 0; nodeIndex < context->getTotalNodes(); nodeIndex++ ){
          partitions.emplace_back(
            std::make_pair(context->getNode(nodeIndex), ral::frame::BlazingTableView(partitioned[nodeIndex], table.names())));
      }
    } else {
      for(int nodeIndex = 0; nodeIndex < context->getTotalNodes(); nodeIndex++ ){
        partitions.emplace_back(
          std::make_pair(context->getNode(nodeIndex), ral::frame::BlazingTableView(table.view(), table.names())));
      }
    }

  	context->incrementQuerySubstep();
    ral::distribution::distributePartitions(context, partitions);
    std::vector<NodeColumn> remote_node_columns = ral::distribution::collectPartitions(context);

    std::vector<ral::frame::BlazingTableView> partitions_to_concat;
    for (int i = 0; i < remote_node_columns.size(); i++){
      partitions_to_concat.emplace_back(remote_node_columns[i].second->toBlazingTableView());
    }
    bool found_self_partition = false;
    for (auto partition : partitions){
      if (partition.first == ral::communication::CommunicationData::getInstance().getSelfNode()){
        partitions_to_concat.emplace_back(partition.second);
        found_self_partition = true;
        break;
      }
    }
    assert(found_self_partition);
    return ral::utilities::concatTables(partitions_to_concat);
  }

bool check_if_has_nulls(CudfTableView const& input, std::vector<cudf::size_type> const& keys){
  auto keys_view = input.select(keys);
  if (keys_view.num_columns() != 0 && keys_view.num_rows() != 0 && cudf::has_nulls(keys_view)) {
      return true;
  }

  return false;
}

} // namespace processor
} // namespace ral
