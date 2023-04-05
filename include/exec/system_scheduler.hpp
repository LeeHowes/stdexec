/*
 * Copyright (c) 2023 Lee Howes
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "stdexec/execution.hpp"
// For the default implementation, test will override
#include "exec/static_thread_pool.hpp"

#include <iostream>

struct __exec_system_scheduler_interface;
struct __exec_system_sender_interface;
struct __exec_system_scheduler_impl;
struct __exec_system_sender_impl;


// Virtual interfaces to underlying implementations for initial simplicit
// TODO: Potentially move to custom vtable implementations
struct __exec_system_context_interface {
  virtual __exec_system_scheduler_interface* get_scheduler() noexcept = 0;
};

// bulk function for scheduler to transmit from, will wrap actual function stub stored in real type
using __exec_system_bulk_shape = long;
using __exec_system_bulk_function = void(__exec_system_bulk_shape);


struct __exec_system_scheduler_interface {
  virtual stdexec::forward_progress_guarantee get_forward_progress_guarantee() const = 0;
  virtual __exec_system_sender_interface* schedule() = 0;
  // TODO: Move chaining in here to support chaining after a system_sender or other system_bulk_sender
  // or don't do anything that specific?
  virtual __exec_system_sender_interface* bulk(__exec_system_bulk_shape shp, __exec_system_bulk_function* fn) = 0;
  virtual bool equals(const __exec_system_scheduler_interface* rhs) const = 0;
};

struct __exec_system_operation_state_interface {
  virtual void start() noexcept = 0;
};

struct __exec_system_receiver {
  void* cpp_recv_ = nullptr;
  void (*set_value)(void* cpp_recv);
  void (*set_stopped)(void* cpp_recv);
  // TODO: set_error
};

struct __exec_system_sender_interface {
  virtual __exec_system_operation_state_interface* connect(__exec_system_receiver recv) noexcept = 0;
  virtual __exec_system_scheduler_interface* get_completion_scheduler() noexcept = 0;
};






// Low-level APIs
// Phase 2 will move to pointers and ref counting ala COM
// Phase 3 will move these to weak symbols and allow replacement in tests
// Default implementation based on static_thread_pool
struct __exec_system_context_impl : public __exec_system_context_interface {
  exec::static_thread_pool pool_;

  __exec_system_scheduler_interface* get_scheduler() noexcept override;
};


struct __exec_system_scheduler_impl : public __exec_system_scheduler_interface {
  __exec_system_scheduler_impl(
      __exec_system_context_impl* ctx, decltype(ctx->pool_.get_scheduler()) pool_scheduler) :
      ctx_{ctx}, pool_scheduler_{pool_scheduler} {}

  __exec_system_context_impl* ctx_;
  decltype(ctx_->pool_.get_scheduler()) pool_scheduler_;

  __exec_system_sender_interface* schedule() override;

  __exec_system_sender_interface* bulk(__exec_system_bulk_shape shp, __exec_system_bulk_function* fn) override;

  stdexec::forward_progress_guarantee get_forward_progress_guarantee() const override {
    return stdexec::forward_progress_guarantee::parallel;
  }

  bool equals(const __exec_system_scheduler_interface* rhs) const override {
    return dynamic_cast<const __exec_system_scheduler_impl*>(rhs) == this;
  }
};

struct __exec_system_operation_state_impl;
using __exec_pool_sender_t = decltype(stdexec::schedule(std::declval<__exec_system_scheduler_impl>().pool_scheduler_));

struct __exec_system_pool_receiver {
  friend void tag_invoke(stdexec::set_value_t, __exec_system_pool_receiver&&) noexcept;

  friend void tag_invoke(stdexec::set_stopped_t, __exec_system_pool_receiver&&) noexcept;

  friend void tag_invoke(stdexec::set_error_t, __exec_system_pool_receiver&&, std::exception_ptr) noexcept {
  }

  friend stdexec::empty_env tag_invoke(stdexec::get_env_t, const __exec_system_pool_receiver&) noexcept {
    return {};
  }

  __exec_system_operation_state_impl* os_ = nullptr;
};

struct __exec_system_operation_state_impl : public __exec_system_operation_state_interface {
  __exec_system_operation_state_impl(
    __exec_pool_sender_t&& pool_sender,
    __exec_system_receiver&& recv) :
    recv_{std::move(recv)},
    pool_operation_state_{
      [&](){return stdexec::connect(std::move(pool_sender), __exec_system_pool_receiver{this});}()} {
  }

  __exec_system_operation_state_impl(const __exec_system_operation_state_impl&) = delete;
  __exec_system_operation_state_impl(__exec_system_operation_state_impl&&) = delete;
  __exec_system_operation_state_impl& operator= (const __exec_system_operation_state_impl&) = delete;
  __exec_system_operation_state_impl& operator= (__exec_system_operation_state_impl&&) = delete;


  void start() noexcept override {
    stdexec::start(pool_operation_state_);
  }

  __exec_system_receiver recv_;
  decltype(stdexec::connect(
      std::move(std::declval<__exec_pool_sender_t>()), std::move(std::declval<__exec_system_pool_receiver>())))
    pool_operation_state_;
};

inline void tag_invoke(stdexec::set_value_t, __exec_system_pool_receiver&& recv) noexcept {
  __exec_system_receiver &system_recv = recv.os_->recv_;
  system_recv.set_value((system_recv.cpp_recv_));
}

inline void tag_invoke(stdexec::set_stopped_t, __exec_system_pool_receiver&& recv) noexcept {
  __exec_system_receiver &system_recv = recv.os_->recv_;
  recv.os_->recv_.set_stopped(&(system_recv.cpp_recv_));
}



struct __exec_system_sender_impl : public __exec_system_sender_interface {
  __exec_system_sender_impl(
        __exec_system_scheduler_impl* scheduler, __exec_pool_sender_t&& pool_sender) :
      scheduler_{scheduler}, pool_sender_(std::move(pool_sender)) {

  }

  __exec_system_operation_state_interface* connect(__exec_system_receiver recv) noexcept override {
    return
      new __exec_system_operation_state_impl(std::move(pool_sender_), std::move(recv));
  }

  __exec_system_scheduler_interface* get_completion_scheduler() noexcept override {
    return scheduler_;
  };

   __exec_system_scheduler_impl* scheduler_;
   __exec_pool_sender_t pool_sender_;
};


struct __exec_system_bulk_operation_state_impl;
struct __exec_system_bulk_pool_receiver {
  friend void tag_invoke(stdexec::set_value_t, __exec_system_bulk_pool_receiver&&) noexcept;

  friend void tag_invoke(stdexec::set_stopped_t, __exec_system_bulk_pool_receiver&&) noexcept;

  friend void tag_invoke(stdexec::set_error_t, __exec_system_bulk_pool_receiver&&, std::exception_ptr) noexcept {
  }

  friend stdexec::empty_env tag_invoke(stdexec::get_env_t, const __exec_system_bulk_pool_receiver&) noexcept {
    return {};
  }

  __exec_system_bulk_operation_state_impl* os_ = nullptr;
};

struct __exec_system_bulk_operation_state_impl : public __exec_system_operation_state_interface {
  __exec_system_bulk_operation_state_impl(
    __exec_pool_sender_t&& pool_sender,
    __exec_system_receiver&& recv) :
    recv_{std::move(recv)},
    // TODO: Clearly this is going to be more sophisticated to launch multiple elements
    // TODO: Call bulk_function_ across pool
    pool_operation_state_{
      [&](){return stdexec::connect(std::move(pool_sender), __exec_system_bulk_pool_receiver{this});}()} {
  }

  __exec_system_bulk_operation_state_impl(const __exec_system_bulk_operation_state_impl&) = delete;
  __exec_system_bulk_operation_state_impl(__exec_system_bulk_operation_state_impl&&) = delete;
  __exec_system_bulk_operation_state_impl& operator= (const __exec_system_bulk_operation_state_impl&) = delete;
  __exec_system_bulk_operation_state_impl& operator= (__exec_system_bulk_operation_state_impl&&) = delete;


  void start() noexcept override {
    stdexec::start(pool_operation_state_);
  }

  __exec_system_receiver recv_;
  __exec_system_bulk_function* bulk_function_;
  decltype(stdexec::connect(
      std::move(std::declval<__exec_pool_sender_t>()), std::move(std::declval<__exec_system_bulk_pool_receiver>())))
    pool_operation_state_;
};

inline void tag_invoke(stdexec::set_value_t, __exec_system_bulk_pool_receiver&& recv) noexcept {
  __exec_system_receiver &system_recv = recv.os_->recv_;
  system_recv.set_value((system_recv.cpp_recv_));
}

inline void tag_invoke(stdexec::set_stopped_t, __exec_system_bulk_pool_receiver&& recv) noexcept {
  __exec_system_receiver &system_recv = recv.os_->recv_;
  recv.os_->recv_.set_stopped(&(system_recv.cpp_recv_));
}

// A bulk sender is just a system sender viewed externally.
// TODO: a bulk operation state is just a system operation state viewed externally
struct __exec_system_bulk_sender_impl : public __exec_system_sender_interface {
  __exec_system_bulk_sender_impl(
        __exec_system_scheduler_impl* scheduler, __exec_pool_sender_t&& pool_sender) :
      scheduler_{scheduler}, pool_sender_(std::move(pool_sender)) {

  }

  __exec_system_operation_state_interface* connect(__exec_system_receiver recv) noexcept override {
    return
      new __exec_system_bulk_operation_state_impl(std::move(pool_sender_), std::move(recv));
  }

  __exec_system_scheduler_interface* get_completion_scheduler() noexcept override {
    return scheduler_;
  };

   __exec_system_scheduler_impl* scheduler_;
   __exec_pool_sender_t pool_sender_;
};


// Phase 1 implementation, single implementation
static __exec_system_context_impl* __get_exec_system_context_impl() {
  static __exec_system_context_impl impl_;

  return &impl_;
}

inline __exec_system_scheduler_interface* __exec_system_context_impl::get_scheduler() noexcept {
  // TODO: ref counting etc
  return new __exec_system_scheduler_impl(this, pool_.get_scheduler());
}

inline __exec_system_sender_interface* __exec_system_scheduler_impl::schedule() {
  return new __exec_system_sender_impl(this, stdexec::schedule(pool_scheduler_));
}


inline __exec_system_sender_interface* __exec_system_scheduler_impl::bulk(
    __exec_system_bulk_shape shp,
    __exec_system_bulk_function* fn) {
  // TODO: Construct bulk sender impl

  // TEMPORARY to get to build
  return new __exec_system_sender_impl(this, stdexec::schedule(pool_scheduler_));
}




namespace exec {
  namespace __system_scheduler {

  } // namespace system_scheduler


  class system_scheduler;
  class system_sender;
  template<stdexec::sender S, std::integral Shape, class Fn>
  class system_bulk_sender;

  class system_context {
  public:
    system_context() {
      impl_ = __get_exec_system_context_impl();
      // TODO error handling
    }

    system_scheduler get_scheduler();

  private:
    __exec_system_context_interface* impl_ = nullptr;

  };

  class system_scheduler {
  public:

    // Pointer that we ref count?
    system_scheduler(__exec_system_scheduler_interface* scheduler_interface) : scheduler_interface_(scheduler_interface) {}

    bool operator==(const system_scheduler& rhs) const noexcept {
      return scheduler_interface_->equals(rhs.scheduler_interface_);
    }

  private:
    friend system_sender tag_invoke(
      stdexec::schedule_t, const system_scheduler&) noexcept;

    friend stdexec::forward_progress_guarantee tag_invoke(
      stdexec::get_forward_progress_guarantee_t,
      const system_scheduler&) noexcept;

    template <stdexec::sender S, std::integral Shape, class Fn>
    friend system_bulk_sender<S, Shape, Fn> tag_invoke(       //
      stdexec::bulk_t,                                        //
      const system_scheduler& sch,                            //
      S&& sndr,                                               //
      Shape shape,                                            //
      Fn fun)                                                 //
      noexcept;

    __exec_system_scheduler_interface* scheduler_interface_;
    friend class system_context;
  };

  class system_sender {
  public:
    using is_sender = void;
    using completion_signatures =
      stdexec::completion_signatures< stdexec::set_value_t(), stdexec::set_stopped_t() >;

    system_sender(__exec_system_sender_interface* sender_impl) : sender_impl_{sender_impl} {}

  private:
    template <class S, class R_>
    struct __op {
      using R = stdexec::__t<R_>;

      template<class F>
      __op(system_sender&& snd, R&& recv, F&& initFunc) :
          snd_{std::move(snd)}, recv_{std::move(recv)}, os_{initFunc(*this)} {
      }
      __op(const __op&) = delete;
      __op(__op&&) = delete;
      __op& operator= (const __op&) = delete;
      __op& operator= (__op&&) = delete;

      friend void tag_invoke(stdexec::start_t, __op& op) noexcept {
        if(auto os = op.os_) {
          os->start();
        }
      }

      S snd_;
      R recv_;
      __exec_system_operation_state_interface* os_ = nullptr;
    };

    template <class R>
    friend auto tag_invoke(stdexec::connect_t, system_sender&& snd, R&& rec) //
      noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<R>, R>)
        -> __op<system_sender, stdexec::__x<std::remove_cvref_t<R>>> {

      return __op<system_sender, stdexec::__x<std::remove_cvref_t<R>>>{
        std::move(snd),
        std::move(rec),
        [](auto& op){
          __exec_system_receiver receiver_impl{
            &op.recv_,
            [](void* cpp_recv){
              stdexec::set_value(std::move(*static_cast<R*>(cpp_recv)));
            },
            [](void* cpp_recv){
              stdexec::set_stopped(std::move(*static_cast<R*>(cpp_recv)));
            }};

          return op.snd_.sender_impl_->connect(std::move(receiver_impl));
        }};
    }

    struct __env {
      friend system_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<stdexec::set_value_t>, const __env& self) //
        noexcept {
        return {self.scheduler_impl_};
      }

      friend system_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>, const __env& self) //
        noexcept {
        return {self.scheduler_impl_};
      }

      __exec_system_scheduler_interface* scheduler_impl_;
    };

    friend __env tag_invoke(stdexec::get_env_t, const system_sender& snd) noexcept {
      return {snd.sender_impl_->get_completion_scheduler()};
    }

    __exec_system_sender_interface* sender_impl_ = nullptr;
  };

   template<stdexec::sender Pred, std::integral Shape, class Fn>
   class system_bulk_sender {
  public:
    using Sender = Pred;
    using Fun = Fn;
    using is_sender = void;
    using completion_signatures =
      stdexec::completion_signatures< stdexec::set_value_t(), stdexec::set_stopped_t() >;

    system_bulk_sender(
      __exec_system_scheduler_interface* scheduler_impl,
      Sender pred,

      Shape shape,
      Fun&& fun) :
      scheduler_impl_{scheduler_impl},
      pred_{std::move(pred)},
      shape_{std::move(shape)},
      fun_{std::move(fun)} {}

  private:
    template <class S, class R_>
    struct __op {
      using R = stdexec::__t<R_>;

      template<class F>
      __op(system_bulk_sender&& snd, R&& recv, F&& initFunc) :
          snd_{std::move(snd)}, recv_{std::move(recv)}, os_{initFunc(*this)} {
      }
      __op(const __op&) = delete;
      __op(__op&&) = delete;
      __op& operator= (const __op&) = delete;
      __op& operator= (__op&&) = delete;

      friend void tag_invoke(stdexec::start_t, __op& op) noexcept {
        if(auto os = op.os_) {
          os->start();
        }
      }

      S snd_;
      R recv_;
      __exec_system_operation_state_interface* os_ = nullptr;
    };

    template <class R>
    friend auto tag_invoke(stdexec::connect_t, system_bulk_sender&& snd, R&& rec) //
      noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<R>, R>)
        -> __op<system_bulk_sender, stdexec::__x<std::remove_cvref_t<R>>> {

      return __op<system_bulk_sender, stdexec::__x<std::remove_cvref_t<R>>>{
        std::move(snd),
        std::move(rec),
        [](auto& op){
          __exec_system_receiver receiver_impl{
            &op.recv_,
            [](void* cpp_recv){
              stdexec::set_value(std::move(*static_cast<R*>(cpp_recv)));
            },
            [](void* cpp_recv){
              stdexec::set_stopped(std::move(*static_cast<R*>(cpp_recv)));
            }};
          // TODO We don't want to do this. We want to turn this into a proper operation that chains on snd
          return op.snd_.scheduler_impl_->schedule()->connect(std::move(receiver_impl));
        }};
    }

    struct __env {
      friend system_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<stdexec::set_value_t>, const __env& self) //
        noexcept {
        return {self.scheduler_impl_};
      }

      friend system_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>, const __env& self) //
        noexcept {
        return {self.scheduler_impl_};
      }


      __exec_system_scheduler_interface* scheduler_impl_;
    };

    friend __env tag_invoke(stdexec::get_env_t, const system_bulk_sender& snd) noexcept {
      // If we trigger this customization we know what the completion scheduler will be
      return {snd.scheduler_impl_};
    }

    __exec_system_scheduler_interface* scheduler_impl_ = nullptr;
    Sender pred_;
    Shape shape_;
    // TODO: Store this in OS and reference it from lambda passed to type erased OS
    Fun fun_;
  };


  inline system_scheduler system_context::get_scheduler() {
    return system_scheduler{impl_->get_scheduler()};
  }

  system_sender tag_invoke(
      stdexec::schedule_t, const system_scheduler& sched) noexcept {
    return system_sender(sched.scheduler_interface_->schedule());
  }

  stdexec::forward_progress_guarantee tag_invoke(
      stdexec::get_forward_progress_guarantee_t,
      const system_scheduler& sched) noexcept {
    return sched.scheduler_interface_->get_forward_progress_guarantee();
  }


  template <stdexec::sender S, std::integral Shape, class Fn>
  system_bulk_sender<S, Shape, Fn> tag_invoke(              //
    stdexec::bulk_t,                                        //
    const system_scheduler& sch,                            //
    S&& pred,                                               //
    Shape shape,                                            //
    Fn fun)                                                 //
    noexcept {
    return system_bulk_sender<S, Shape, Fn>{
      sch.scheduler_interface_, (S&&) pred, shape, (Fn&&) fun};
  }

} // namespace exec
