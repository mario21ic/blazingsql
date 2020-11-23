#pragma once

#include "BlazingColumn.h"
#include "LogicPrimitives.h"
#include "CacheMachine.h"
#include "io/DataLoader.h"
#include "io/Schema.h"
#include "utilities/CommonOperations.h"

#include "parser/expression_utils.hpp"

#include <cudf/types.hpp>
#include "execution_graph/logic_controllers/LogicalProject.h"
#include <execution_graph/logic_controllers/LogicPrimitives.h>
#include <src/execution_graph/logic_controllers/LogicalFilter.h>

#include <src/operators/OrderBy.h>
#include <src/operators/GroupBy.h>
#include <src/utilities/DebuggingUtils.h>
#include <stack>
#include <mutex>
#include "io/DataLoader.h"
#include "io/Schema.h"
#include "io/data_parser/CSVParser.h"
#include <Util/StringUtil.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/foreach.hpp>
#include "parser/expression_utils.hpp"

#include <cudf/copying.hpp>
#include <cudf/merge.hpp>
#include <cudf/search.hpp>
#include <cudf/sorting.hpp>
#include <src/CalciteInterpreter.h>
#include <src/utilities/CommonOperations.h>

#include "distribution/primitives.h"
#include "config/GPUManager.cuh"
#include "CacheMachine.h"
#include "ExceptionHandling/BlazingThread.h"

#include "taskflow/graph.h"
#include "communication/CommunicationData.h"
#include "execution_graph/logic_controllers/taskflow/kernel.h"
#include "CodeTimer.h"

namespace ral {
namespace batch {

using ral::cache::kstatus;
using ral::cache::kernel;
using ral::cache::kernel_type;
using namespace fmt::literals;

using RecordBatch = std::unique_ptr<ral::frame::BlazingTable>;
using frame_type = std::vector<std::unique_ptr<ral::frame::BlazingTable>>;
using Context = blazingdb::manager::Context;

/**
 * @brief This is the standard data sequencer that just pulls data from an input cache one batch at a time.
 */
class BatchSequence {
public:
	/**
	 * Constructor for the BatchSequence
	 * @param cache The input cache from where the data will be pulled.
	 * @param kernel The kernel that will actually receive the pulled data.
	 * @param ordered Indicates whether the order should be kept at data pulling.
	 */
	BatchSequence(std::shared_ptr<ral::cache::CacheMachine> cache = nullptr, const ral::cache::kernel * kernel = nullptr, bool ordered = true)
	: cache{cache}, kernel{kernel}, ordered{ordered}
	{}

	/**
	 * Updates the input cache machine.
	 * @param cache The pointer to the new input cache.
	 */
	void set_source(std::shared_ptr<ral::cache::CacheMachine> cache) {
		this->cache = cache;
	}

	/**
	 * Get the next message as a unique pointer to a BlazingTable.
	 * If there are no more messages on the queue we get a nullptr.
	 * @return Unique pointer to a BlazingTable containing the next decached message.
	 */
	RecordBatch next() {
		std::shared_ptr<spdlog::logger> cache_events_logger;
		cache_events_logger = spdlog::get("cache_events_logger");

		CodeTimer cacheEventTimer(false);

		cacheEventTimer.start();
		std::unique_ptr<ral::frame::BlazingTable> output;
		if (ordered) {
			output = cache->pullFromCache();
		} else {
			output = cache->pullUnorderedFromCache();
		}
		cacheEventTimer.stop();

		if(output){
			auto num_rows = output->num_rows();
			auto num_bytes = output->sizeInBytes();

			if(cache_events_logger != nullptr) {
				cache_events_logger->info("{ral_id}|{query_id}|{source}|{sink}|{num_rows}|{num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
							"ral_id"_a=cache->get_context()->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
							"query_id"_a=cache->get_context()->getContextToken(),
							"source"_a=cache->get_id(),
							"sink"_a=kernel->get_id(),
							"num_rows"_a=num_rows,
							"num_bytes"_a=num_bytes,
							"event_type"_a="removeCache",
							"timestamp_begin"_a=cacheEventTimer.start_time(),
							"timestamp_end"_a=cacheEventTimer.end_time());
			}
		}

		return output;
	}

	/**
	 * Blocks executing thread until a new message is ready or when the message queue is empty.
	 * @return true A new message is ready.
	 * @return false There are no more messages on the cache.
	 */
	bool wait_for_next() {
		if (kernel) {
			std::string message_id = std::to_string((int)kernel->get_type_id()) + "_" + std::to_string(kernel->get_id());
		}

		return cache->wait_for_next();
	}

	/**
	 * Indicates if the message queue is not empty at this point on time.
	 * @return true There is at least one message in the queue.
	 * @return false Message queue is empty.
	 */
	bool has_next_now() {
		return cache->has_next_now();
	}
private:
	std::shared_ptr<ral::cache::CacheMachine> cache; /**< Cache machine from which the data will be pulled. */
	const ral::cache::kernel * kernel; /**< Pointer to the kernel that will receive the cache data. */
	bool ordered; /**< Indicates whether the order should be kept when pulling data from the cache. */
};

/**
 * @brief This data sequencer works as a bypass to take data from one input to an output without decacheing.
 */
class BatchSequenceBypass {
public:
	/**
	 * Constructor for the BatchSequenceBypass
	 * @param cache The input cache from where the data will be pulled.
	 * @param kernel The kernel that will actually receive the pulled data.
	 */
	BatchSequenceBypass(std::shared_ptr<ral::cache::CacheMachine> cache = nullptr, const ral::cache::kernel * kernel = nullptr)
	: cache{cache}, kernel{kernel}
	{}

	/**
	 * Updates the input cache machine.
	 * @param cache The pointer to the new input cache.
	 */
	void set_source(std::shared_ptr<ral::cache::CacheMachine> cache) {
		this->cache = cache;
	}

	/**
	 * Get the next message as a CacheData object.
	 * @return CacheData containing the next message without decacheing.
	 */
	std::unique_ptr<ral::cache::CacheData> next() {
		std::shared_ptr<spdlog::logger> cache_events_logger;
		cache_events_logger = spdlog::get("cache_events_logger");

		CodeTimer cacheEventTimer(false);

		cacheEventTimer.start();
		auto output = cache->pullCacheData();
		cacheEventTimer.stop();

		if (output) {
			auto num_rows = output->num_rows();
			auto num_bytes = output->sizeInBytes();

			cache_events_logger->info("{ral_id}|{query_id}|{source}|{sink}|{num_rows}|{num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
							"ral_id"_a=cache->get_context()->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
							"query_id"_a=cache->get_context()->getContextToken(),
							"source"_a=cache->get_id(),
							"sink"_a=kernel->get_id(),
							"num_rows"_a=num_rows,
							"num_bytes"_a=num_bytes,
							"event_type"_a="removeCache",
							"timestamp_begin"_a=cacheEventTimer.start_time(),
							"timestamp_end"_a=cacheEventTimer.end_time());
		}

		return output;
	}

	/**
	 * Blocks executing thread until a new message is ready or when the message queue is empty.
	 * @return true A new message is ready.
	 * @return false There are no more messages on the cache.
	 */
	bool wait_for_next() {
		return cache->wait_for_next();
	}

	/**
	 * Indicates if the message queue is not empty at this point on time.
	 * @return true There is at least one message in the queue.
	 * @return false Message queue is empty.
	 */
	bool has_next_now() {
		return cache->has_next_now();
	}
private:
	std::shared_ptr<ral::cache::CacheMachine> cache; /**< Cache machine from which the data will be pulled. */
	const ral::cache::kernel * kernel; /**< Pointer to the kernel that will receive the cache data. */
};



/**
 * @brief Gets data from a data source, such as a set of files or from a DataFrame.
 * These data sequencers are used by the TableScan's.
 */
class DataSourceSequence {
public:
	/**
	 * Constructor for the DataSourceSequence
	 * @param loader Data loader responsible for executing the batching load.
	 * @param schema Table schema associated to the data to be loaded.
	 * @param context Shared context associated to the running query.
	 */
	DataSourceSequence(ral::io::data_loader &loader, ral::io::Schema & schema, std::shared_ptr<Context> context)
		: context(context), loader(loader), schema(schema), batch_index{0}, n_batches{0}
	{
		// n_partitions{n_partitions}: TODO Update n_batches using data_loader
		this->provider = loader.get_provider();
		this->parser = loader.get_parser();

		n_files = schema.get_files().size();
		for (size_t index = 0; index < n_files; index++) {
			all_row_groups.push_back(schema.get_rowgroup_ids(index));
		}

		is_empty_data_source = (n_files == 0 && parser->get_num_partitions() == 0);
		is_gdf_parser = parser->get_num_partitions() > 0;
		if(is_gdf_parser){
			n_batches = std::max(parser->get_num_partitions(), (size_t)1);
		} else if (parser->type() == ral::io::DataType::CSV)	{
			auto csv_parser = static_cast<ral::io::csv_parser*>(parser.get());

			n_batches = 0;
			size_t max_bytes_chuck_size = csv_parser->max_bytes_chuck_size();
			if (max_bytes_chuck_size > 0) {
				int file_idx = 0;
				while (provider->has_next()) {
					auto data_handle = provider->get_next();
					int64_t file_size = data_handle.file_handle->GetSize().ValueOrDie();
					size_t num_chunks = (file_size + max_bytes_chuck_size - 1) / max_bytes_chuck_size;
					std::vector<int> file_row_groups(num_chunks);
					std::iota(file_row_groups.begin(), file_row_groups.end(), 0);
					all_row_groups[file_idx] = std::move(file_row_groups);
					n_batches += num_chunks;
					file_idx++;
				}
				provider->reset();
			} else {
				n_batches = n_files;
			}
		}	else {
			n_batches = 0;
			for (auto &&row_group : all_row_groups) {
				n_batches += std::max(row_group.size(), (size_t)1);
			}
		}

		if (!is_empty_data_source && !is_gdf_parser && has_next()) {
			current_data_handle = provider->get_next();
		}
	}

	/**
	 * Get the next batch as a unique pointer to a BlazingTable.
	 * If there are no more batches we get a nullptr.
	 * @return Unique pointer to a BlazingTable containing the next batch read.
	 */
	RecordBatch next() {
		std::unique_lock<std::mutex> lock(mutex_);

		if (!has_next()) {
			return nullptr;
		}

		if (is_empty_data_source) {
			batch_index++;
			return schema.makeEmptyBlazingTable(projections);
		}

		if(is_gdf_parser){
//			auto ret = loader.load_batch(context.get(), projections, schema, ral::io::data_handle(), 0, {static_cast<cudf::size_type>(batch_index)} );
			batch_index++;

//			return std::move(ret);
		}

		auto local_cur_data_handle = current_data_handle;
		auto local_cur_file_index = cur_file_index;
		auto local_all_row_groups = all_row_groups[cur_file_index];
		if (file_batch_index > 0 && file_batch_index >= local_all_row_groups.size()) {
			// a file handle that we can use in case errors occur to tell the user which file had parsing issues
			assert(provider->has_next());
			current_data_handle = provider->get_next();

			file_batch_index = 0;
			cur_file_index++;

			local_cur_data_handle = current_data_handle;
			local_cur_file_index = cur_file_index;
			local_all_row_groups = all_row_groups[cur_file_index];
		}

		std::vector<cudf::size_type> local_row_group;
		if (!local_all_row_groups.empty()) {
			local_row_group = { local_all_row_groups[file_batch_index] };
		}

		file_batch_index++;
		batch_index++;

		lock.unlock();

		try {
//			return loader.load_batch(context.get(), projections, schema, local_cur_data_handle, local_cur_file_index, local_row_group);
		}	catch(const std::exception& e) {
			auto logger = spdlog::get("batch_logger");
			logger->error("{query_id}|||{info}|||||",
										"query_id"_a=context->getContextToken(),
										"info"_a="In DataSourceSequence while reading file {}. What: {}"_format(local_cur_data_handle.uri.toString(), e.what()));
			throw;
		}
	}

	/**
	 * Indicates if there are more batches to process.
	 * @return true There is at least one batch to be processed.
	 * @return false The data source is empty or all batches have already been processed.
	 */
	bool has_next() {
		return (is_empty_data_source && batch_index < 1) || (batch_index.load() < n_batches);
	}

	/**
	 * Updates the set of columns to be projected at the time of reading the data source.
	 * @param projections The set of column ids to be selected.
	 */
	void set_projections(std::vector<int> projections) {
		this->projections = projections;
	}

	/**
	 * Get the batch index.
	 * @note This function can be called from a parallel thread, so we want it to be thread safe.
	 * @return The current batch index.
	 */
	size_t get_batch_index() {
		return batch_index.load();
	}

	/**
	 * Get the number of batches identified on the data source.
	 * @return The number of batches.
	 */
	size_t get_num_batches() {
		return n_batches;
	}

private:
	std::shared_ptr<ral::io::data_provider> provider; /**< Data provider associated to the data loader. */
	std::shared_ptr<ral::io::data_parser> parser; /**< Data parser associated to the data loader. */

	std::shared_ptr<Context> context; /**< Pointer to the shared query context. */
	std::vector<int> projections; /**< List of columns that will be selected if they were previously settled. */
	ral::io::data_loader loader; /**< Data loader responsible for executing the batching load. */
	ral::io::Schema  schema; /**< Table schema associated to the data to be loaded. */
	std::vector<std::vector<int>> all_row_groups;
	std::atomic<size_t> batch_index; /**< Current global batch index. */
	size_t n_batches; /**< Number of batches. */
	size_t n_files; /**< Number of files. */
	bool is_empty_data_source; /**< Indicates whether the data source is empty. */
	bool is_gdf_parser; /**< Indicates whether the parser is a gdf one. */
	int cur_file_index{}; /**< Current file index. */
	int file_batch_index{};  /**< Current file batch index. */
	ral::io::data_handle current_data_handle{};

	std::mutex mutex_; /**< Mutex for making the loading batch thread-safe. */
};

/**
 * @brief This kernel loads the data from the specified data source.
 */
class TableScan : public kernel {
public:
	/**
	 * Constructor for TableScan
	 * @param kernel_id Kernel identifier.
	 * @param queryString Original logical expression that the kernel will execute.
	 * @param loader Data loader responsible for executing the batching load.
	 * @param schema Table schema associated to the data to be loaded.
	 * @param context Shared context associated to the running query.
	 * @param query_graph Shared pointer of the current execution graph.
	 */

	TableScan(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<ral::io::data_provider> provider, std::shared_ptr<ral::io::data_parser> parser, ral::io::Schema & schema, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
	: kernel(kernel_id, queryString, context, kernel_type::TableScanKernel),schema(schema), provider(provider), parser(parser)
	{



		size_t n_batches;
		if(parser->type() == ral::io::DataType::CUDF){
			n_batches = std::max(provider->get_num_handles(), (size_t)1);
		} else if (parser->type() == ral::io::DataType::CSV)	{
			auto csv_parser = static_cast<ral::io::csv_parser*>(parser.get());
			n_batches = 0;
			size_t max_bytes_chuck_size = csv_parser->max_bytes_chuck_size();
			if (max_bytes_chuck_size > 0) {
				int file_idx = 0;
				while (provider->has_next()) {
					auto data_handle = provider->get_next();
					int64_t file_size = data_handle.file_handle->GetSize().ValueOrDie();
					size_t num_chunks = (file_size + max_bytes_chuck_size - 1) / max_bytes_chuck_size;
					std::vector<int> file_row_groups(num_chunks);
					std::iota(file_row_groups.begin(), file_row_groups.end(), 0);
					schema.get_rowgroups()[file_idx] = std::move(file_row_groups);
					n_batches += num_chunks;
					file_idx++;
				}
				provider->reset();
			} else {
				n_batches = provider->get_num_handles();
			}
		}	else {
			n_batches = 0;
			for (auto row_group : schema.get_rowgroups()) {
				n_batches += std::max(row_group.size(), (size_t)1);
			}
		}
		num_batches = (double)n_batches;

		this->query_graph = query_graph;
	}

	
	void do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
		std::shared_ptr<ral::cache::CacheMachine> output,
		cudaStream_t stream, std::string kernel_process_name) override{
			//for now the output kernel is not using do_process
			//i believe the output should be a cachemachine itself
			//obviating this concern
		output->addToCache(std::move(inputs[0]), std::string(""));

		
	}

	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		CodeTimer timer;



		std::vector<int> projections(schema.get_num_columns());
		std::iota(projections.begin(), projections.end(), 0);

		cudf::size_type current_rows = 0;
		
		//if its empty we can just add it to the cache without scheduling
		if (!provider->has_next()) {
			this->add_to_output_cache(std::move(schema.makeEmptyBlazingTable(projections)));
			return kstatus::proceed;
		}




		while(provider->has_next()){
			//retrieve the file handle but do not open the file
			//this will allow us to prevent from having too many open file handles by being
			//able to limit the number of file tasks
			auto handle = provider->get_next(true);
			auto file_schema = schema.fileSchema(file_index);
			auto row_group_ids = schema.get_rowgroup_ids(file_index);
			//this is the part where we make the task now
			std::unique_ptr<ral::cache::CacheData> input = 
				std::make_unique<ral::cache::CacheDataIO>(handle,parser,schema,file_schema,row_group_ids,projections);
			std::vector<std::unique_ptr<ral::cache::CacheData> > inputs;
			inputs.push_back(std::move(input));
			auto output_cache = this->output_.get_cache(std::to_string(this->get_id()));
			add_task(
				ral::execution::executor::get_instance()->add_task(
					std::move(inputs),
					output_cache,
					this,std::string("scan")
				)
			);

			if (this->has_limit_ && output_cache->get_num_rows_added() >= this->limit_rows_) {
			//	break;
			}
			file_index++;
		}

		/*std::vector<BlazingThread> threads;
		for (int i = 0; i < table_scan_kernel_num_threads; i++) {
			threads.push_back(BlazingThread([this, &has_limit, &limit_, &current_rows]() {
				CodeTimer eventTimer(false);

				std::unique_ptr<ral::frame::BlazingTable> batch;
				while(batch = input.next()) {
					eventTimer.start();
					eventTimer.stop();
					current_rows += batch->num_rows();

					events_logger->info("{ral_id}|{query_id}|{kernel_id}|{input_num_rows}|{input_num_bytes}|{output_num_rows}|{output_num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
									"ral_id"_a=context->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
									"query_id"_a=context->getContextToken(),
									"kernel_id"_a=this->get_id(),
									"input_num_rows"_a=batch->num_rows(),
									"input_num_bytes"_a=batch->sizeInBytes(),
									"output_num_rows"_a=batch->num_rows(),
									"output_num_bytes"_a=batch->sizeInBytes(),
									"event_type"_a="compute",
									"timestamp_begin"_a=eventTimer.start_time(),
									"timestamp_end"_a=eventTimer.end_time());

					this->add_to_output_cache(std::move(batch));
					
					if (has_limit && current_rows >= limit_) {
						break;
					}
				}
			}));
		}
		for (auto &&t : threads) {
			t.join();
		}
*/
		logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
									"query_id"_a=context->getContextToken(),
									"step"_a=context->getQueryStep(),
									"substep"_a=context->getQuerySubstep(),
									"info"_a="TableScan Kernel Completed",
									"duration"_a=timer.elapsed_time(),
									"kernel_id"_a=this->get_id());

		std::unique_lock<std::mutex> lock(kernel_mutex);
		kernel_cv.wait(lock,[this]{
			return this->tasks.empty();
		});
		return kstatus::proceed;
	}

	/**
	 * Returns the estimated num_rows for the output at one point.
	 * @return A pair representing that there is no data to be processed, or the estimated number of output rows.
	 */
	virtual std::pair<bool, uint64_t> get_estimated_output_num_rows(){

		double rows_so_far = (double)this->output_.total_rows_added();
		
		double current_batch = (double)file_index;
		if (current_batch == 0 || num_batches == 0){
			return std::make_pair(false, 0);
		} else {
			return std::make_pair(true, (uint64_t)(rows_so_far/(current_batch/num_batches)));
		}
	}

private:
	std::shared_ptr<ral::io::data_provider> provider;
	std::shared_ptr<ral::io::data_parser> parser;
	ral::io::Schema  schema; /**< Table schema associated to the data to be loaded. */ 
	size_t file_index = 0;
	double num_batches;


// std::shared_ptr<Context> context; /**< Pointer to the shared query context. */
// 	std::vector<int> projections; /**< List of columns that will be selected if they were previously settled. */
// 	ral::io::data_loader loader; /**< Data loader responsible for executing the batching load. */
// 	ral::io::Schema  schema; /**< Table schema associated to the data to be loaded. */
// 	size_t cur_file_index; /**< Current file index. */
// 	size_t cur_row_group_index; /**< Current rowgroup index. */
// 	std::vector<std::vector<int>> all_row_groups;
// 	std::atomic<size_t> batch_index; /**< Current batch index. */
// 	size_t n_batches; /**< Number of batches. */
// 	size_t n_files; /**< Number of files. */
// 	bool is_empty_data_source; /**< Indicates whether the data source is empty. */
// 	bool is_gdf_parser; /**< Indicates whether the parser is a gdf one. */

// 	std::mutex mutex_; /**< Mutex for making the loading batch thread-safe. */

};

/**
 * @brief This kernel loads the data and delivers only columns that are requested.
 * It also filters the data if there are one or more filters, and sets their column aliases
 * accordingly.
 */
class BindableTableScan : public kernel {
public:
	/**
	 * Constructor for BindableTableScan
	 * @param kernel_id Kernel identifier.
	 * @param queryString Original logical expression that the kernel will execute.
	 * @param loader Data loader responsible for executing the batching load.
	 * @param schema Table schema associated to the data to be loaded.
	 * @param context Shared context associated to the running query.
	 * @param query_graph Shared pointer of the current execution graph.
	 */
	BindableTableScan(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<ral::io::data_provider> provider, std::shared_ptr<ral::io::data_parser> parser, ral::io::Schema & schema, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
	: kernel(kernel_id, queryString, context, kernel_type::TableScanKernel),schema(schema), provider(provider), parser(parser)
	{
		this->query_graph = query_graph;
		this->filtered = is_filtered_bindable_scan(expression);
	}

	
	void do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
		std::shared_ptr<ral::cache::CacheMachine> output,
		cudaStream_t stream, std::string kernel_process_name) override{
			//for now the output kernel is not using do_process
			//i believe the output should be a cachemachine itself
			//obviating this concern
		
		auto & input = inputs[0];			
		if(this->filtered) {

			auto columns = ral::processor::process_filter(input->toBlazingTableView(), expression, this->context.get());
			columns->setNames(fix_column_aliases(columns->names(), expression));

			output->addToCache(std::move(columns), std::string(""));
		}else{
			input->setNames(fix_column_aliases(input->names(), expression));
			output->addToCache(std::move(input), std::string(""));

		}
	}

	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		CodeTimer timer;


		std::vector<int> projections = get_projections(expression);
		if(projections.size() == 0){
			projections.resize(schema.get_num_columns());
			std::iota(projections.begin(), projections.end(), 0);
		}


		cudf::size_type current_rows = 0;
		
		//if its empty we can just add it to the cache without scheduling
		if (!provider->has_next()) {
			auto empty = schema.makeEmptyBlazingTable(projections);
			empty->setNames(fix_column_aliases(empty->names(), expression));
			this->add_to_output_cache(std::move(empty));
			return kstatus::proceed;
		}




		while(provider->has_next()){
			//retrieve the file handle but do not open the file
			//this will allow us to prevent from having too many open file handles by being
			//able to limit the number of file tasks
			auto handle = provider->get_next(true);
			auto file_schema = schema.fileSchema(file_index);
			auto row_group_ids = schema.get_rowgroup_ids(file_index);
			//this is the part where we make the task now
			std::unique_ptr<ral::cache::CacheData> input = 
				std::make_unique<ral::cache::CacheDataIO>(handle,parser,schema,file_schema,row_group_ids,projections);
			std::vector<std::unique_ptr<ral::cache::CacheData> > inputs;
			inputs.push_back(std::move(input));

			auto output_cache = this->output_.get_cache(std::to_string(this->get_id()));
			add_task(
				ral::execution::executor::get_instance()->add_task(
					std::move(inputs),
					output_cache,
					this,std::string("scan")
				)
			);

			file_index++;
			if (this->has_limit_ && output_cache->get_num_rows_added() >= this->limit_rows_) {
			//	break;
			}

		}

		logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
									"query_id"_a=context->getContextToken(),
									"step"_a=context->getQueryStep(),
									"substep"_a=context->getQuerySubstep(),
									"info"_a="BindableTableScan Kernel Completed",
									"duration"_a=timer.elapsed_time(),
									"kernel_id"_a=this->get_id());
		std::unique_lock<std::mutex> lock(kernel_mutex);
		kernel_cv.wait(lock,[this]{
			return this->tasks.empty();
		});
		return kstatus::proceed;
	}

	/**
	 * Returns the estimated num_rows for the output at one point.
	 * @return A pair representing that there is no data to be processed, or the estimated number of output rows.
	 */
	virtual std::pair<bool, uint64_t> get_estimated_output_num_rows(){

		double rows_so_far = (double)this->output_.total_rows_added();
		
		double current_batch = (double)file_index;
		if (current_batch == 0 || num_batches == 0){
			return std::make_pair(false, 0);
		} else {
			return std::make_pair(true, (uint64_t)(rows_so_far/(current_batch/num_batches)));
		}
	}
private:
	std::shared_ptr<ral::io::data_provider> provider;
	std::shared_ptr<ral::io::data_parser> parser;
	ral::io::Schema  schema; /**< Table schema associated to the data to be loaded. */ 
	size_t file_index = 0;
	double num_batches;
	bool filtered;
};

/**
 * @brief This kernel only returns the subset columns contained in the logical projection expression.
 */
class Projection : public kernel {
public:
	/**
	 * Constructor for Projection
	 * @param kernel_id Kernel identifier.
	 * @param queryString Original logical expression that the kernel will execute.
	 * @param context Shared context associated to the running query.
	 * @param query_graph Shared pointer of the current execution graph.
	 */
	Projection(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
	: kernel(kernel_id, queryString, context, kernel_type::ProjectKernel)
	{
		this->query_graph = query_graph;
	}

	void do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
		std::shared_ptr<ral::cache::CacheMachine> output,
		cudaStream_t stream, std::string kernel_process_name) override{
			//for now the output kernel is not using do_process
			//i believe the output should be a cachemachine itself
			//obviating this concern
		auto & input = inputs[0];
		auto columns = ral::processor::process_project(std::move(input), expression, this->context.get());
		output->addToCache(std::move(columns), std::string(""));
	}

	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		CodeTimer timer;
		CodeTimer eventTimer(false);

		std::unique_ptr <ral::cache::CacheData> cache_data =  this->input_cache()->pullCacheData();
		while(cache_data != nullptr ){
			std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
			inputs.push_back(std::move(cache_data));
			auto output_cache = this->output_.get_cache(std::to_string(this->get_id()));
			add_task(
				ral::execution::executor::get_instance()->add_task(
					std::move(inputs),
					output_cache,
					this,std::string("filter")
				)
			);
			cache_data =  this->input_cache()->pullCacheData();
		}

		std::unique_lock<std::mutex> lock(kernel_mutex);
		kernel_cv.wait(lock,[this]{
			return this->tasks.empty();
		});

		if(logger != nullptr) {
			logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
									"query_id"_a=context->getContextToken(),
									"step"_a=context->getQueryStep(),
									"substep"_a=context->getQuerySubstep(),
									"info"_a="Projection Kernel Completed",
									"duration"_a=timer.elapsed_time(),
									"kernel_id"_a=this->get_id());
		}
		return kstatus::proceed;
	}

private:

};

/**
 * @brief This kernel filters the data according to the specified conditions.
 */
class Filter : public kernel {
public:
	/**
	 * Constructor for TableScan
	 * @param kernel_id Kernel identifier.
	 * @param queryString Original logical expression that the kernel will execute.
	 * @param context Shared context associated to the running query.
	 * @param query_graph Shared pointer of the current execution graph.
	 */
	Filter(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
	: kernel(kernel_id, queryString, context, kernel_type::FilterKernel)
	{
		this->query_graph = query_graph;
	}


void do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
		std::shared_ptr<ral::cache::CacheMachine> output,
		cudaStream_t stream, std::string kernel_process_name) override{
			//for now the output kernel is not using do_process
			//i believe the output should be a cachemachine itself
			//obviating this concern
		auto & input = inputs[0];
		auto columns = ral::processor::process_filter(input->toBlazingTableView(), expression, this->context.get());
		output->addToCache(std::move(columns), std::string(""));
	}

	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		CodeTimer timer;
		CodeTimer eventTimer(false);

		std::unique_ptr <ral::cache::CacheData> cache_data =  this->input_cache()->pullCacheData();
		while(cache_data != nullptr ){
			std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
			inputs.push_back(std::move(cache_data));
			auto output_cache = this->output_.get_cache(std::to_string(this->get_id()));
			add_task(
				ral::execution::executor::get_instance()->add_task(
					std::move(inputs),
					output_cache,
					this,std::string("filter")
				)
			);
			cache_data =  this->input_cache()->pullCacheData();
		}

		std::unique_lock<std::mutex> lock(kernel_mutex);
		kernel_cv.wait(lock,[this]{
			return this->tasks.empty();
		});

		logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
									"query_id"_a=context->getContextToken(),
									"step"_a=context->getQueryStep(),
									"substep"_a=context->getQuerySubstep(),
									"info"_a="Filter Kernel Completed",
									"duration"_a=timer.elapsed_time(),
									"kernel_id"_a=this->get_id());

		return kstatus::proceed;
	}

	/**
	 * Returns the estimated num_rows for the output at one point.
	 * @return A pair representing that there is no data to be processed, or the estimated number of output rows.
	 */
	std::pair<bool, uint64_t> get_estimated_output_num_rows(){
		std::pair<bool, uint64_t> total_in = this->query_graph->get_estimated_input_rows_to_kernel(this->kernel_id);
		if (total_in.first){
			double out_so_far = (double)this->output_.total_rows_added();
			double in_so_far = (double)this->input_.total_rows_added();
			if (in_so_far == 0){
				return std::make_pair(false, 0);
			} else {
				return std::make_pair(true, (uint64_t)( ((double)total_in.second) *out_so_far/in_so_far) );
			}
		} else {
			return std::make_pair(false, 0);
		}
	}

private:

};

/**
 * @brief This kernel allows printing the preceding input caches to the standard output.
 */
class Print : public kernel {
public:
	/**
	 * Constructor
	 */
	Print() : kernel(0,"Print", nullptr, kernel_type::PrintKernel) { ofs = &(std::cout); }
	Print(std::ostream & stream) : kernel(0,"Print", nullptr, kernel_type::PrintKernel) { ofs = &stream; }

	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		std::lock_guard<std::mutex> lg(print_lock);
		BatchSequence input(this->input_cache(), this);
		while (input.wait_for_next() ) {
			auto batch = input.next();
			ral::utilities::print_blazing_table_view(batch->toBlazingTableView());
		}
		return kstatus::stop;
	}

protected:
	std::ostream * ofs = nullptr; /**< Target output stream object. */
	std::mutex print_lock; /**< Mutex for making the printing thread-safe. */
};


/**
 * @brief This kernel represents the last step of the execution graph.
 * Basically it allows to extract the result of the different levels of
 * memory abstractions in the form of a concrete table.
 */
class OutputKernel : public kernel {
public:
	/**
	 * Constructor for OutputKernel
	 * @param kernel_id Kernel identifier.
	 * @param context Shared context associated to the running query.
	 */
	OutputKernel(std::size_t kernel_id, std::shared_ptr<Context> context) : kernel(kernel_id,"OutputKernel", context, kernel_type::OutputKernel) { }

	void do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
		std::shared_ptr<ral::cache::CacheMachine> output,
		cudaStream_t stream, std::string kernel_process_name) override{
			//for now the output kernel is not using do_process
			//i believe the output should be a cachemachine itself
			//obviating this concern
			
		}
	/**
	 * Executes the batch processing.
	 * Loads the data from their input port, and after processing it,
	 * the results are stored in their output port.
	 * @return kstatus 'stop' to halt processing, or 'proceed' to continue processing.
	 */
	virtual kstatus run() {
		while (this->input_.get_cache()->wait_for_next()) {
			CodeTimer cacheEventTimer(false);

			cacheEventTimer.start();
			auto temp_output = std::move(this->input_.get_cache()->pullFromCache());
			cacheEventTimer.stop();

			if(temp_output){
				auto num_rows = temp_output->num_rows();
				auto num_bytes = temp_output->sizeInBytes();

				cache_events_logger->info("{ral_id}|{query_id}|{source}|{sink}|{num_rows}|{num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
								"ral_id"_a=context->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
								"query_id"_a=context->getContextToken(),
								"source"_a=this->input_.get_cache()->get_id(),
								"sink"_a=this->get_id(),
								"num_rows"_a=num_rows,
								"num_bytes"_a=num_bytes,
								"event_type"_a="removeCache",
								"timestamp_begin"_a=cacheEventTimer.start_time(),
								"timestamp_end"_a=cacheEventTimer.end_time());

				output.emplace_back(std::move(temp_output));
			}
		}

		return kstatus::stop;
	}

	
	/**
	 * Returns the vector containing the final processed output.
	 * @return frame_type A vector of unique_ptr of BlazingTables.
	 */
	frame_type release() {
		return std::move(output);
	}

protected:
	frame_type output; /**< Vector of tables with the final output. */
};

} // namespace batch
} // namespace ral
