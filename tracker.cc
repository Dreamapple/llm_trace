
#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

#include "butil/status.h"
#include "bthread/bthread.h"
#include "bthread/butex.h"
#include "bthread/condition_variable.h"

#include <brpc/controller.h>
#include <brpc/channel.h>

using namespace nlohmann;
using butil::Status;


class BThread {
public:
    BThread() = default;

    template< class Function, class... Args >
    explicit BThread( Function&& fn, Args&&... args ) {
        auto p_wrap_fn = new auto([=]{ fn(args...);   });
        auto call_back = [](void* ar) ->void* {
            auto f = reinterpret_cast<decltype(p_wrap_fn)>(ar);
            (*f)();
            delete f;
            return nullptr;
        };
        bthread_start_background(&bthid_, nullptr, call_back, (void*)p_wrap_fn);
    }

    int join() {
        printf("[%p] BThread join...\n", this);
        if (bthid_ != INVALID_BTHREAD) {
            return bthread_join(bthid_, NULL);
        }
        return -1;
    }

    bthread_t get_tid() {
        return bthid_;
    }

private:
    bthread_t bthid_ = INVALID_BTHREAD;
};

bthread_key_t& TrackerKey() {
    static bthread_key_t key = []() {
        bthread_key_t k;
        bthread_key_create(&k, nullptr);
        return k;
    }();
    return key;
}

void* active_tracker() {
    return bthread_getspecific(TrackerKey());
}

void push_tracker(void* this_tracker) {
    bthread_setspecific(TrackerKey(), this_tracker);
}

void pop_tracker(void* this_tracker) {
    assert(active_tracker() == this_tracker);
    bthread_setspecific(TrackerKey(), nullptr);
}

class TrackerReporter {
public:
    struct TrackerReporterConfig
    {
        bool enable_report = false;
        std::string report_host = "http://127.0.0.1:9123";
    };

    int Init(TrackerReporterConfig config) {
        config_ = config;
        if (config_.enable_report) {
            brpc::ChannelOptions opt;
            opt.protocol = brpc::PROTOCOL_HTTP;
            return chann_.Init(config_.report_host.c_str(), "", &opt);
        }
        return 0;
    }

    int report_trace(const std::string& payload) {
        if (!config_.enable_report) {
            return 0;
        }

        brpc::Controller ctl;
        ctl.http_request().set_method(brpc::HttpMethod::HTTP_METHOD_POST);
        ctl.request_attachment().append(payload);
        chann_.CallMethod(NULL, &ctl, NULL, NULL, NULL);
        int status_code = ctl.http_response().status_code();
        return status_code;
    }
private:
    brpc::Channel chann_;
    TrackerReporterConfig config_;
};

TrackerReporter& tracker_reporter() {
    static TrackerReporter reporter;
    return reporter;
}


class TrackerCallGuardBase {
public:
    explicit TrackerCallGuardBase(TrackerCallGuardBase* parent, const std::string& scope_name) : parent_(parent), scope_name_(scope_name) {
        printf("[%p] Construct TrackerCallGuardBase(parent=%p, scope_name=%s)\n", this, parent, scope_name.c_str());
        push_tracker(this);
        call_info_["bthread_id"] = bthread_self();
        call_info_["call_ts"] = butil::gettimeofday_ms();
        call_info_["scope_name"] = scope_name;
    }
    virtual ~TrackerCallGuardBase() {
        call_info_["ret_ts"] = butil::gettimeofday_ms();
        printf("[%p] Deconstruct TrackerCallGuardBase buf=%s\n", this, call_info_.dump().c_str());
        if (parent_) {
            parent_->collect_subroutine(call_info_);
            pop_tracker(this);
            push_tracker(parent_);
        } else {
            report();
            pop_tracker(this);
        }
    }

    std::string scope_name() {
        return scope_name_;
    }

    std::string parent_scope_name() {
        if (parent_) {
            return parent_->scope_name();
        }
        return "<No Parent>";
    }

    TrackerCallGuardBase* parent() {
        return parent_;
    }

    void collect(json& event) {
        printf("[%p] TrackerCallGuardBase::collect event=%s\n", this, event.dump().c_str());
        call_info_["events"].push_back(event);
    }

    void collect_subroutine(json& event) {
        printf("[%p] TrackerCallGuardBase::collect_subroutine event=%s\n", this, event.dump().c_str());
        call_info_["subroutines"].push_back(event);
    }

    void dump() {
        call_info_["dump_ts"] = butil::gettimeofday_ms();
        printf("[%p] TrackerCallGuardBase::dump\n", this);
        std::ofstream ofs("tracker.json");
        ofs << call_info_.dump(4);
        ofs.close();
    }
private:
    void report() {
        tracker_reporter().report_trace(call_info_.dump());
    }

private:
    TrackerCallGuardBase* parent_;
    std::string scope_name_;
    static brpc::Channel chann_;

protected:
    json call_info_;
};

template<class Use, class ...Args>
class TrackerCallGuard : public TrackerCallGuardBase {
public:
    // 这里传入的Tracker是父bthread的tracker, 作用是获取 caller 的名称
    TrackerCallGuard(TrackerCallGuardBase* tracker, Use& use, Args& ...args)
        : TrackerCallGuardBase(tracker, use.name()), use_(use), args_(std::make_tuple(std::ref(args)...))
    {
        printf("[%p] Construct TrackerCallGuard(tracker=%p, use=%s, args=...)\n", this, tracker, use.name().c_str());
        
        call_info_["caller"] = parent_scope_name();
        call_info_["callee"] = use.name();
        call_info_["callee_clazz"] = use.type();
        call_info_["params_in"] = make_params();
    }
    ~TrackerCallGuard() {
        printf("[%p] Deconstruct TrackerCallGuard\n", this);
        call_info_["params_out"] = make_params();
    }
private:
    json make_params() {
        // printf("[%p] make_params\n", this);
        return make_params_helper(std::make_index_sequence<sizeof...(Args)>());
    }

    template<size_t... Is>
    json make_params_helper(std::index_sequence<Is...>) {
        // printf("[%p] make_params_helper\n", this);
        json params;
        (make_one_param(params, std::get<Is>(args_), Is), ...);
        return params;
    }

    template<class T>
    void make_one_param(json& params, T& arg, int index) {
        // printf("[%p] make_params_helper\n");
        params[index] = arg;
    }

private:
    Use& use_;
    std::tuple<Args&...> args_;
};

template<class T>
class UseFuture {
public:

private:
    T value;
};


template<class T>
class SimpleFuture {
public:
    class __Base {
    public:
        void set_bthread(BThread& b) {
            b_ = b;
        }
        void set_value(T value) {
            value_ = value;
        }
        void wait() {
            printf("[%p] SimpleFuture::__Base wait...\n", this);
            b_.join();
        }
    private:
        BThread b_;
        T value_;
    };

    SimpleFuture(std::shared_ptr<__Base> base) : __base(base) {}
    void wait() {
        printf("[%p] SimpleFuture wait...\n", this);
        return __base->wait();
    }

private:
    std::shared_ptr<__Base> __base;
};


template<class Use>
class GoCtx {
public:
    GoCtx(Use& use) : use_(use) {
        printf("[%p] Construct GoCtx\n", this);
    }

    ~GoCtx() {
        printf("[%p] Destruct GoCtx\n", this);
    }

    template<class ...Args>
    SimpleFuture<Status> operator()(Args && ...args) {
        printf("[%p] GoCtx::operator()\n", this);
        TrackerCallGuardBase* tracker = (TrackerCallGuardBase*)active_tracker(); // 在外部获取，从而捕获BThread切换信息.
        printf("[%p] GoCtx::operator() active_tracker()->%p\n", this, tracker);

        auto fut = std::make_shared<SimpleFuture<Status>::__Base>();

        BThread b([&, fut, tracker] {
            TrackerCallGuard<Use, Args...> guard(tracker, use_, args...);
            auto ret = use_->Run(args...);
            fut->set_value(ret);
        });
        fut->set_bthread(b);
        return SimpleFuture<Status>(fut);
    }

private:
    Use& use_;
};

class Go {
public:
    template<class T>
    GoCtx<T> operator[](T&& t) {
        return GoCtx<T>(t);
    }
};

Go go;

template<class T>
class Use {
public:
    Use(const std::string& name) : name_(name) {}

    template<class ...Args>
    Status operator()(Args && ...args) {
        TrackerCallGuardBase* tracker = (TrackerCallGuardBase*)active_tracker();
        {
            TrackerCallGuard<Use, Args...> guard(tracker, *this, args...);
            return (*this)->Run(args...); 
        }
    }

    T* operator->() {
        return &t;
    }

    std::string name() {
        return name_;
    }

    std::string type() {
        return typeid(T).name();
    }

    T t;
    std::string name_;
};

class Worker1 {
public:
    Status Run(int a, int b, int& c) {
        c = a + b;
        return Status::OK();
    }
};


class Worker2 {
public:
    Status Run(int a, int b, std::string& c) {
        int cc = 0;
        go[worker1](a, b, cc).wait();
        c = std::to_string(a) + "+" + std::to_string(b) + "=" + std::to_string(cc);
        return Status::OK();
    }
private:
    Use<Worker1> worker1 = Use<Worker1>("worker1");
};



Use<Worker2> worker2("worker2");


int main() {
    tracker_reporter().Init({.enable_report = true});

    TrackerCallGuardBase guard(nullptr, "root");
    int a = 1;
    int b = 2;
    std::string c;
    auto f = go[worker2](a, b, c);
    f.wait();
    printf("c=%s\n", c.c_str());

    std::string cc;
    auto s = worker2(a, b, cc);
    printf("cc=%s\n", cc.c_str());

    guard.dump();
}
