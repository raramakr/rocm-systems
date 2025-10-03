// MIT License
//
// Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "lib/rocprofiler-sdk/spm/spm_dispatch_handlers.hpp"

#include "lib/common/container/small_vector.hpp"
#include "lib/common/synchronized.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/buffer.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
//#include "lib/rocprofiler-sdk/counters/sample_processing.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/kernel_dispatch/profiling_time.hpp"

#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/rocprofiler.h>

namespace rocprofiler
{
namespace SPM
{

void
start_context(const context::context* ctx)
{
    if(!ctx || !ctx->dispatch_spm) return;

    auto* controller = hsa::get_queue_controller();

    bool already_enabled = true;
    CHECK_NOTNULL(controller)->enable_serialization();
    ctx->dispatch_spm->enabled.wlock([&](auto& enabled) {
        if(enabled) return;
        already_enabled = false;
        enabled         = true;
    });

    if(!already_enabled)
    {
        for(auto& cb : ctx->dispatch_spm->callbacks)
        {
            // Insert our callbacks into HSA Interceptor. This
            // turns on counter instrumentation.
            if(cb->queue_id != rocprofiler::hsa::ClientID{-1}) continue;
            cb->queue_id = controller->add_callback(
                std::nullopt,
                [=](const hsa::Queue&                                               q,
                    const hsa::rocprofiler_packet&                                  kern_pkt,
                    rocprofiler_kernel_id_t                                         kernel_id,
                    rocprofiler_dispatch_id_t                                       dispatch_id,
                    rocprofiler_user_data_t*                                        user_data,
                    const hsa::Queue::queue_info_session_t::external_corr_id_map_t& extern_corr_ids,
                    const context::correlation_id* correlation_id) {
                    return pre_kernel_call(ctx,
                                    cb,
                                    q,
                                    kern_pkt,
                                    kernel_id,
                                    dispatch_id,
                                    user_data,
                                    extern_corr_ids,
                                    correlation_id);
                },
                // Completion CB
                [=](const hsa::Queue& /* q */,
                    hsa::rocprofiler_packet /* kern_pkt */,
                    std::shared_ptr<hsa::Queue::queue_info_session_t>& session,
                    inst_pkt_t&                                        aql,
                    kernel_dispatch::profiling_time                    dispatch_time) {
                    post_kernel_call(ctx, cb, session, aql, dispatch_time);
                });
        }
    }
}

void
stop_context(const context::context* ctx)
{
    if(!ctx || !ctx->dispatch_spm) return;

    auto* controller = hsa::get_queue_controller();

    ctx->dispatch_spm->enabled.wlock([&](auto& enabled) {
        if(!enabled) return;
        enabled = false;
    });

    if(controller) controller->disable_serialization();

}

bool
AsyncSignalHandler(hsa_signal_value_t /*signal_v*/, void* data)
{
    auto* packet     = CHECK_NOTNULL(static_cast<hsa::SPMPacket*>(data));
    auto& before_krn = packet->before_krn_pkt;

    if(before_krn.size() < 2)
    {
        ROCP_ERROR << "Invalid before_krn packet" << std::endl;
        return true;
    }

    packet->kfd_start();

    get_core().hsa_signal_destroy_fn(before_krn.at(0).barrier_and.completion_signal);
    get_core().hsa_signal_store_screlease_fn(before_krn.at(1).barrier_and.dep_signal[0], 0);
    return false;
}

/**
 * Callback we get from HSA interceptor when a kernel packet is being enqueued.
 * We return an AQLPacket containing the start/stop/read packets for injection.
 */
hsa::Queue::pkt_and_serialize_t
pre_kernel_call(const context::context*        ctx,
                  const std::shared_ptr<spm_counter_callback_info>&  info,  
                  const hsa::Queue&              queue,
                  const hsa::rocprofiler_packet& pkt,
                  uint64_t                       kernel_id,
                  rocprofiler_dispatch_id_t                                       dispatch_id,
                  rocprofiler_user_data_t*                                        user_data,
                  const hsa::Queue::queue_info_session_t::external_corr_id_map_t& extern_corr_ids,
                  const context::correlation_id* correlation_id)
{
    CHECK(info && ctx);

    if(!ctx || !ctx->dispatch_spm) return {nullptr, false};
    
    bool is_enabled = false;
    ctx->dispatch_spm->enabled.rlock(
        [&](const auto& collect_ctx) { is_enabled = collect_ctx; });

    if(!is_enabled || !info->user_cb) return {nullptr, false};
    
    auto _corr_id_v =
        rocprofiler_async_correlation_id_t{.internal = 0, .external = context::null_user_data};
    if(const auto* _corr_id = correlation_id)
    {
        _corr_id_v.internal = _corr_id->internal;
        if(const auto* external =
               rocprofiler::common::get_val(extern_corr_ids, info->internal_context))
        {
            _corr_id_v.external = *external;
        }
    }

    auto req_profile = rocprofiler_spm_counter_config_id_t{.handle = 0};
    auto dispatch_data =
        common::init_public_api_struct(rocprofiler_spm_dispatch_counting_service_data_t{});

    dispatch_data.correlation_id = _corr_id_v;
    {
        auto dispatch_info = common::init_public_api_struct(rocprofiler_kernel_dispatch_info_t{});
        dispatch_info.kernel_id            = kernel_id;
        dispatch_info.dispatch_id          = dispatch_id;
        dispatch_info.agent_id             = CHECK_NOTNULL(queue.get_agent().get_rocp_agent())->id;
        dispatch_info.queue_id             = queue.get_id();
        dispatch_info.private_segment_size = pkt.kernel_dispatch.private_segment_size;
        dispatch_info.group_segment_size   = pkt.kernel_dispatch.group_segment_size;
        dispatch_info.workgroup_size       = {pkt.kernel_dispatch.workgroup_size_x,
                                        pkt.kernel_dispatch.workgroup_size_y,
                                        pkt.kernel_dispatch.workgroup_size_z};
        dispatch_info.grid_size            = {pkt.kernel_dispatch.grid_size_x,
                                   pkt.kernel_dispatch.grid_size_y,
                                   pkt.kernel_dispatch.grid_size_z};
        dispatch_data.dispatch_info        = dispatch_info;
    }

    auto ret = info->user_cb(dispatch_data, &req_profile, user_data, info->callback_args);

    if(ret && req_profile.handle == 0) return {nullptr, true};

    auto prof_config = spm_get_controller().get_profile_cfg(req_profile);
    CHECK(prof_config);
    
    std::unique_ptr<rocprofiler::hsa::AQLPacket> ret_pkt;
    auto ret_status = info->get_spm_packet(ret_pkt, prof_config, dispatch_data, user_data);
    CHECK_EQ(ret_status, ROCPROFILER_STATUS_SUCCESS) << rocprofiler_get_status_string(ret_status);

    if(!ret_pkt->empty)
    {
        ret_pkt->clear();
        ret_pkt->populate_before();
        ret_pkt->populate_after();
        ROCP_FATAL_IF(ret_pkt->before_krn_pkt.size() < 3) << "SPM Requires at least 3 packets";
        auto& signal_to_start_kfd    = ret_pkt->before_krn_pkt.at(0).barrier_and.completion_signal;
        auto& signal_kfd_has_started = ret_pkt->before_krn_pkt.at(1).barrier_and.dep_signal[0];

        queue.create_signal(0, &signal_to_start_kfd);
        queue.create_signal(0, &signal_kfd_has_started);

        get_core().hsa_signal_store_screlease_fn(signal_kfd_has_started, -1);
        get_core().hsa_signal_store_screlease_fn(signal_to_start_kfd, 0);

        auto status = get_ext().hsa_amd_signal_async_handler_fn(
        signal_to_start_kfd, HSA_SIGNAL_CONDITION_EQ, -1, rocprofiler::SPM::AsyncSignalHandler, ret_pkt.get());
        ROCP_FATAL_IF(status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK)
           << "Error: hsa_amd_signal_async_handler failed with error code " << status
           << " :: " << hsa::get_hsa_status_string(status);

    }
    return {std::move(ret_pkt), true};
}

/**
 * Callback called by HSA interceptor when the kernel has completed processing.
 */
void
post_kernel_call(const context::context*                            ctx,
             const std::shared_ptr<spm_counter_callback_info>&      info,
             std::shared_ptr<hsa::Queue::queue_info_session_t>& /*ptr_session*/,
             inst_pkt_t&                                        pkts,
             kernel_dispatch::profiling_time                   /*dispatch_time*/)
{
    CHECK(info && ctx);

    std::shared_ptr<spm_counter_config> prof_config;
    // Get the Profile Config
    info->packet_return_map.wlock([&](auto& data) {
        for(auto& [aql_pkt, _] : pkts)
        {
            const auto& profile = rocprofiler::common::get_val(data, aql_pkt.get());
            if(profile)
            {
                prof_config = *profile;
                data.erase(aql_pkt.get());
                get_core().hsa_signal_destroy_fn(aql_pkt->before_krn_pkt.at(1).barrier_and.dep_signal[0]);
                prof_config->packets.wlock([&](auto& pkt_vector) { pkt_vector.emplace_back(std::move(aql_pkt)); });
                return;
            }
        }
    });
}
}
}