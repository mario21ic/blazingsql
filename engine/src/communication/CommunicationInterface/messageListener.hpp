#pragma once

#include <memory>
#include <map>

#include "utilities/ctpl_stl.h"
#include "ExceptionHandling/BlazingThread.h"
#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>
#include "messageReceiver.hpp"
#include <mutex>

namespace comm {

class message_listener{
public:
    message_listener(const std::map<std::string, comm::node>& nodes, int num_threads, std::shared_ptr<ral::cache::CacheMachine> input_cache) : pool{num_threads}, _nodes_info_map{nodes}, input_cache{input_cache} {}
    virtual ~message_listener(){

    }
    virtual void start_polling() = 0;
    ctpl::thread_pool<BlazingThread> & get_pool();
    std::map<std::string, comm::node> get_node_map();
    std::shared_ptr<ral::cache::CacheMachine> get_input_cache(){
		return input_cache;
	}

protected:
    ctpl::thread_pool<BlazingThread> pool;
    std::map<std::string, comm::node> _nodes_info_map;
    bool polling_started{false};
    std::shared_ptr<ral::cache::CacheMachine> input_cache;
};

class tcp_message_listener : public message_listener {

public:
    static void initialize_message_listener(const std::map<std::string, comm::node>& nodes, int port, int num_threads, std::shared_ptr<ral::cache::CacheMachine> input_cache);
    static tcp_message_listener * get_instance();
    void start_polling() override;
    int get_port() {
        return _port;
    }
    virtual ~tcp_message_listener(){

    }
private:
    tcp_message_listener(const std::map<std::string, comm::node>& nodes, int port, int num_threads, std::shared_ptr<ral::cache::CacheMachine> input_cache);
    int _port;
    static tcp_message_listener * instance;
};



class ucx_message_listener : public message_listener {
public:

    static void initialize_message_listener(ucp_context_h context, ucp_worker_h worker, const std::map<std::string, comm::node>& nodes, int num_threads, std::shared_ptr<ral::cache::CacheMachine> input_cache);
    static ucx_message_listener * get_instance();
    void poll_begin_message_tag(bool running_from_unit_test);
    void add_receiver(ucp_tag_t tag, std::shared_ptr<message_receiver> receiver);
    std::shared_ptr<message_receiver> get_receiver(ucp_tag_t tag);
    void remove_receiver(ucp_tag_t tag);
    ucp_worker_h get_worker();
    void start_polling() override;
private:
    ucx_message_listener(ucp_context_h context, ucp_worker_h worker, const std::map<std::string, comm::node>& nodes, int num_threads, std::shared_ptr<ral::cache::CacheMachine> input_cache);
	virtual ~ucx_message_listener(){

    }
    void poll_message_tag(ucp_tag_t tag, ucp_tag_t mask);
    size_t _request_size;
    ucp_worker_h ucp_worker;
    std::map<ucp_tag_t,std::shared_ptr<message_receiver> > tag_to_receiver;
	static ucx_message_listener * instance;
    std::mutex receiver_mutex;
};

} // namespace comm
