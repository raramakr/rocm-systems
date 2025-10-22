// MIT License
//
// Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "core/trace_cache/perfetto_post_processing.hpp"
#include "config.hpp"
#include "core/agent_manager.hpp"
#include "library/tracing.hpp"
#include "rocprofiler-sdk/fwd.h"
#include "trace_cache/metadata_registry.hpp"
#include "trace_cache/sample_type.hpp"
#include "trace_cache/storage_parser.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

#if ROCPROFSYS_USE_ROCM > 0
#    include "library/rocprofiler-sdk/fwd.hpp"
#    include <rocprofiler-sdk/context.h>
#endif

namespace rocprofsys
{
namespace trace_cache
{
namespace
{
auto&
get_perfetto_tmp_file()
{
    static std::shared_ptr<tmp_file> _tmp_file = nullptr;
    return _tmp_file;
}

int
get_perffeto_temp_fd(const std::string& _pid)
{
    auto& _tmp_file = get_perfetto_tmp_file();
    if(config::get_use_tmp_files())
    {
        auto _base = JOIN("-", "cached-perfetto-trace", _pid);
        _tmp_file  = config::get_tmp_file(_base, "proto");
        _tmp_file->open(O_RDWR | O_CREAT | O_TRUNC, 0600);
    }
    return ((_tmp_file) ? _tmp_file->fd : -1);
}

auto&
get_perfetto_config()
{
    static auto _v = ::perfetto::TraceConfig{};
    return _v;
}

#if ROCPROFSYS_USE_ROCM > 0
rocprofiler_sdk::function_args_t
process_arguments_string(const std::string& arg_str)
{
    rocprofiler_sdk::function_args_t args;
    const std::string                delimiter = ";;";

    auto split = [](const std::string& str, const std::string& _delimiter) {
        std::vector<std::string> tokens;
        size_t                   start = 0;
        size_t                   end   = str.find(_delimiter);

        while(end != std::string::npos)
        {
            tokens.push_back(str.substr(start, end - start));
            start = end + _delimiter.length();
            end   = str.find(_delimiter, start);
        }

        return tokens;
    };

    auto tokens = split(arg_str, delimiter);

    // Ensure the number of tokens is a multiple of 4
    if(tokens.size() % 4 != 0)
    {
        throw std::invalid_argument("Malformed argument string.");
    }

    for(auto it = tokens.begin(); it != tokens.end(); it += 4)
    {
        rocprofiler_sdk::argument_info arg = { static_cast<uint32_t>(std::stoi(*it)),
                                               *(it + 1), *(it + 2), *(it + 3) };
        args.push_back(arg);
    }

    return args;
}
#endif
}  // namespace

perfetto_post_processing::perfetto_post_processing(metadata_registry& metadata,
                                                   const uint64_t&    pid,
                                                   agent_manager&     agent_mngr)
: m_metadata(metadata)
, m_tracing_session(nullptr)
, m_process_id(pid)
, m_agent_manager(agent_mngr)
{}

void
perfetto_post_processing::setup_perfetto()
{
    auto  args            = ::perfetto::TracingInitArgs{};
    auto  track_event_cfg = ::perfetto::protos::gen::TrackEventConfig{};
    auto& cfg             = get_perfetto_config();

    // environment settings
    auto shmem_size_hint = config::get_perfetto_shmem_size_hint();
    auto buffer_size     = config::get_perfetto_buffer_size();
    auto flush_period    = config::get_perfetto_flush_period();

    auto _policy =
        config::get_perfetto_fill_policy() == "discard"
            ? ::perfetto::protos::gen::TraceConfig_BufferConfig_FillPolicy_DISCARD
            : ::perfetto::protos::gen::TraceConfig_BufferConfig_FillPolicy_RING_BUFFER;
    auto* buffer_config = cfg.add_buffers();
    buffer_config->set_size_kb(buffer_size);
    buffer_config->set_fill_policy(_policy);

    for(const auto& itr : config::get_disabled_categories())
    {
        ROCPROFSYS_VERBOSE_F(1, "Disabling perfetto track event category: %s\n",
                             itr.c_str());
        track_event_cfg.add_disabled_categories(itr);
    }

    cfg.set_flush_period_ms(flush_period);

    auto* ds_cfg = cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("track_event");  // this MUST be track_event
    ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());

    args.shmem_size_hint_kb = shmem_size_hint;
    args.backends = ::perfetto::kInProcessBackend;  // Only in-process for post-processing

    ::perfetto::Tracing::Initialize(args);
    ::perfetto::TrackEvent::Register();
}

void
perfetto_post_processing::start_session()
{
    if(!config::get_use_perfetto()) return;

    if(config::get_perfetto_backend() != "inprocess") return;

    if(!m_tracing_session)
    {
        m_tracing_session = ::perfetto::Tracing::NewTrace();
    }

    ROCPROFSYS_VERBOSE(2,
                       "Starting perfetto post-processing session with cached data...\n");

    auto& cfg     = get_perfetto_config();
    auto  temp_fd = get_perffeto_temp_fd(std::to_string(m_process_id));

    m_tracing_session->Setup(cfg, temp_fd);
    m_tracing_session->StartBlocking();
}

void
perfetto_post_processing::stop_session()
{
    if(!m_tracing_session) return;

    ROCPROFSYS_VERBOSE(2, "Stopping perfetto post-processing session...\n");
    ::perfetto::TrackEvent::Flush();
    m_tracing_session->FlushBlocking();
    m_tracing_session->StopBlocking();
}

void
perfetto_post_processing::post_process(bool& _perfetto_output_error)
{
    using char_vec_t = std::vector<char>;

    if(!m_tracing_session) return;

    stop_session();

    auto _get_session_data = [this]() {
        auto _data     = char_vec_t{};
        auto _tmp_file = get_perfetto_tmp_file();
        if(_tmp_file && *_tmp_file)
        {
            _tmp_file->close();
            FILE* _fdata = ::fopen(_tmp_file->filename.c_str(), "rb");

            if(!_fdata)
            {
                ROCPROFSYS_VERBOSE(
                    -1, "Error! perfetto temp trace file '%s' could not be read",
                    _tmp_file->filename.c_str());
                return char_vec_t{ m_tracing_session->ReadTraceBlocking() };
            }

            ::fseek(_fdata, 0, SEEK_END);
            size_t _fnum_elem = ::ftell(_fdata);
            ::fseek(_fdata, 0, SEEK_SET);

            _data.resize(_fnum_elem, '\0');
            auto _fnum_read = ::fread(_data.data(), sizeof(char), _fnum_elem, _fdata);
            ::fclose(_fdata);

            ROCPROFSYS_CI_THROW(
                _fnum_read != _fnum_elem,
                "Error! read %zu elements from perfetto trace file '%s'. Expected %zu\n",
                _fnum_read, _tmp_file->filename.c_str(), _fnum_elem);
        }
        else
        {
            _data = char_vec_t{ m_tracing_session->ReadTraceBlocking() };
        }

        return _data;
    };

    auto trace_data = char_vec_t{};
    trace_data      = _get_session_data();

    auto output_dir = filepath::dirname(config::get_perfetto_output_filename());
    auto _filename =
        JOIN("/", output_dir,
             JOIN("-", "perfetto-cached-trace", std::to_string(m_process_id) + ".proto"));

    if(!trace_data.empty())
    {
        ROCPROFSYS_VERBOSE(1, "Writing perfetto trace data (%zu bytes)...\n",
                           trace_data.size());

        operation::file_output_message<tim::project::rocprofsys> _fom{};
        if(config::get_verbose() >= 0)
            _fom(_filename, std::string{ "perfetto" },
                 " (%.2f KB / %.2f MB / %.2f GB)... ",
                 static_cast<double>(trace_data.size()) / units::KB,
                 static_cast<double>(trace_data.size()) / units::MB,
                 static_cast<double>(trace_data.size()) / units::GB);

        std::ofstream ofs{};
        if(!filepath::open(ofs, _filename, std::ios::out | std::ios::binary))
        {
            ROCPROFSYS_VERBOSE(-1, "Error opening '%s'...", _filename.c_str());
            _perfetto_output_error = true;
        }
        else
        {
            ofs.write(trace_data.data(), trace_data.size());
            if(config::get_verbose() >= 0) _fom.append("%s", "Done");  // NOLINT
            ROCPROFSYS_VERBOSE(0, "Perfetto trace written to: %s (%.2f MB)\n",
                               _filename.c_str(),
                               static_cast<double>(trace_data.size()) / units::MB);
        }
        ofs.close();
    }
    else
    {
        ROCPROFSYS_VERBOSE(
            0, "perfetto trace data is empty. File '%s' will not be written...\n",
            _filename.c_str());
    }

    // Clean up temp file
    auto& _tmp_file = get_perfetto_tmp_file();
    if(_tmp_file)
    {
        _tmp_file->close();
        _tmp_file->remove();
        _tmp_file.reset();
    }

    // Clean up session
    m_tracing_session.reset();
}

postprocessing_callback
perfetto_post_processing::get_kernel_dispatch_callback() const
{
    return [&]([[maybe_unused]] const storage_parsed_type_base& parsed) {
#if ROCPROFSYS_USE_ROCM > 0
        auto _kds = static_cast<const struct kernel_dispatch_sample&>(parsed);

        auto kernel_symbol = m_metadata.get_kernel_symbol(_kds.kernel_id);
        auto _agent_device_id =
            m_agent_manager.get_agent_by_handle(_kds.agent_id_handle).device_id;
        auto _queue_id_handle = _kds.queue_id_handle;
        auto _stream_handle   = _kds.stream_handle;
        auto _corr_id         = _kds.correlation_id_internal;
        auto _beg_ts          = _kds.start_timestamp;
        auto _end_ts          = _kds.end_timestamp;

        if(!kernel_symbol.has_value())
        {
            throw std::runtime_error("Kernel symbol is missing for kernel dispatch");
        }

        auto kernel_name = tim::demangle(kernel_symbol->kernel_name);

        auto _track_desc = [](uint64_t _device_id_v, uint64_t _queue_id_v) {
            return JOIN("", "GPU Kernel Dispatch [", _device_id_v, "] Queue ",
                        _queue_id_v);
        };

        const auto _track =
            tracing::get_perfetto_track(category::rocm_kernel_dispatch{}, _track_desc,
                                        _agent_device_id, _queue_id_handle);

        auto add_annotations = [&](::perfetto::EventContext ctx) {
            if(config::get_perfetto_annotations())
            {
                tracing::add_perfetto_annotation(ctx, "begin_ns", _beg_ts);
                tracing::add_perfetto_annotation(ctx, "end_ns", _end_ts);
                tracing::add_perfetto_annotation(ctx, "corr_id", _corr_id);
                tracing::add_perfetto_annotation(ctx, "stream_id", _stream_handle);

                tracing::add_perfetto_annotation(ctx, "queue", _queue_id_handle);
                tracing::add_perfetto_annotation(ctx, "dispatch_id", _kds.dispatch_id);
                tracing::add_perfetto_annotation(ctx, "kernel_id", _kds.kernel_id);
                tracing::add_perfetto_annotation(ctx, "private_segment_size",
                                                 _kds.private_segment_size);
                tracing::add_perfetto_annotation(ctx, "group_segment_size",
                                                 _kds.group_segment_size);
                tracing::add_perfetto_annotation(
                    ctx, "workgroup_size",
                    JOIN("", "(",
                         JOIN(',', _kds.workgroup_size_x, _kds.workgroup_size_y,
                              _kds.workgroup_size_z),
                         ")"));
                tracing::add_perfetto_annotation(
                    ctx, "grid_size",
                    JOIN("", "(",
                         JOIN(',', _kds.grid_size_x, _kds.grid_size_y, _kds.grid_size_z),
                         ")"));
            }
        };

        tracing::push_perfetto(category::rocm_kernel_dispatch{}, kernel_name.c_str(),
                               _track, _beg_ts, ::perfetto::Flow::ProcessScoped(_corr_id),
                               add_annotations);

        tracing::pop_perfetto(category::rocm_kernel_dispatch{}, kernel_name.c_str(),
                              _track, _end_ts);
#endif
    };
}

postprocessing_callback
perfetto_post_processing::get_memory_copy_callback() const
{
    return [&]([[maybe_unused]] const storage_parsed_type_base& parsed) {
#if ROCPROFSYS_USE_ROCM > 0
        auto _mcs = static_cast<const struct memory_copy_sample&>(parsed);

        auto _corr_id   = _mcs.correlation_id_internal;
        auto _thrd_id   = _mcs.thread_id;
        auto _stream_id = _mcs.stream_handle;
        auto _beg_ts    = _mcs.start_timestamp;
        auto _end_ts    = _mcs.end_timestamp;

        auto _src_agent_log_node_id =
            m_agent_manager.get_agent_by_handle(_mcs.src_agent_id_handle).logical_node_id;
        auto _dst_agent_log_node_id =
            m_agent_manager.get_agent_by_handle(_mcs.dst_agent_id_handle).logical_node_id;
        auto _name = std::string{ m_metadata.get_buffer_name_info().at(
            static_cast<rocprofiler_buffer_tracing_kind_t>(_mcs.kind),
            static_cast<rocprofiler_tracing_operation_t>(_mcs.operation)) };

        auto _track_desc = [](int32_t _device_id_v, rocprofiler_thread_id_t _tid) {
            const auto& _tid_v = thread_info::get(_tid, SystemTID);
            return JOIN("", "GPU Memory Copy to Agent [", _device_id_v, "] Thread ",
                        _tid_v->index_data->sequent_value);
        };

        const auto _track = tracing::get_perfetto_track(
            category::rocm_memory_copy{}, _track_desc, _dst_agent_log_node_id, _thrd_id);

        auto add_perfetto_annotations = [&](::perfetto::EventContext ctx) {
            if(config::get_perfetto_annotations())
            {
                tracing::add_perfetto_annotation(ctx, "begin_ns", _beg_ts);
                tracing::add_perfetto_annotation(ctx, "end_ns", _end_ts);
                tracing::add_perfetto_annotation(ctx, "corr_id", _corr_id);
                tracing::add_perfetto_annotation(ctx, "stream_id", _stream_id);
                tracing::add_perfetto_annotation(ctx, "dst_agent",
                                                 _dst_agent_log_node_id);
                tracing::add_perfetto_annotation(ctx, "src_agent",
                                                 _src_agent_log_node_id);
            }
        };

        tracing::push_perfetto(category::rocm_memory_copy{}, _name.c_str(), _track,
                               _beg_ts, ::perfetto::Flow::ProcessScoped(_corr_id),
                               add_perfetto_annotations);
        tracing::pop_perfetto(category::rocm_memory_copy{}, "", _track, _end_ts);
#endif
    };
}

#if(ROCPROFSYS_USE_ROCM > 0 && ROCPROFILER_VERSION >= 600)
postprocessing_callback
perfetto_post_processing::get_memory_allocate_callback() const
{
#    if ROCPROFSYS_USE_ROCM > 0
    auto memop_to_string =
        [](rocprofiler_memory_allocation_operation_t op) -> const char* {
        switch(op)
        {
            case ROCPROFILER_MEMORY_ALLOCATION_NONE: return "NONE";
            case ROCPROFILER_MEMORY_ALLOCATION_ALLOCATE: return "ALLOCATE";
            case ROCPROFILER_MEMORY_ALLOCATION_VMEM_ALLOCATE: return "VMEM_ALLOCATE";
            case ROCPROFILER_MEMORY_ALLOCATION_FREE: return "FREE";
            case ROCPROFILER_MEMORY_ALLOCATION_VMEM_FREE: return "VMEM_FREE";
            default: return "UNKNOWN";
        }
    };
#    endif
    return [&]([[maybe_unused]] const storage_parsed_type_base& parsed) {
#    if ROCPROFSYS_USE_ROCM > 0
        auto _mas = static_cast<const struct memory_allocate_sample&>(parsed);

        auto _thrd_id    = _mas.thread_id;
        auto _corr_id    = _mas.correlation_id_internal;
        auto _stream_id  = _mas.stream_handle;
        auto _beg_ts     = _mas.start_timestamp;
        auto _end_ts     = _mas.end_timestamp;
        auto _addr_val   = _mas.address_value;
        auto _alloc_size = _mas.allocation_size;

        const auto invalid_context = ROCPROFILER_CONTEXT_NONE;
        if(_mas.agent_id_handle != invalid_context.handle)
        {
            const auto* operation = memop_to_string(
                static_cast<rocprofiler_memory_allocation_operation_t>(_mas.operation));

            auto _track_desc = [](int32_t _device_id_v, rocprofiler_thread_id_t _tid) {
                const auto& _tid_v = thread_info::get(_tid, SystemTID);
                return JOIN("", "GPU Memory Allocation to Agent [", _device_id_v,
                            "] Thread ", _tid_v->index_data->sequent_value);
            };

            auto _agent_logical_node_id =
                m_agent_manager.get_agent_by_handle(_mas.agent_id_handle).logical_node_id;

            const auto _track =
                tracing::get_perfetto_track(category::rocm_memory_allocate{}, _track_desc,
                                            _agent_logical_node_id, _thrd_id);

            auto add_perfetto_annotations = [&](::perfetto::EventContext ctx) {
                if(config::get_perfetto_annotations())
                {
                    tracing::add_perfetto_annotation(ctx, "begin_ns", _beg_ts);
                    tracing::add_perfetto_annotation(ctx, "end_ns", _end_ts);
                    tracing::add_perfetto_annotation(ctx, "corr_id", _corr_id);
                    tracing::add_perfetto_annotation(ctx, "stream_id", _stream_id);
                    tracing::add_perfetto_annotation(ctx, "agent",
                                                     _agent_logical_node_id);
                    tracing::add_perfetto_annotation(ctx, "address_value", _addr_val);
                    tracing::add_perfetto_annotation(ctx, "allocation_size", _alloc_size);
                }
            };

            tracing::push_perfetto(category::rocm_memory_allocate{}, operation, _track,
                                   _beg_ts, ::perfetto::Flow::ProcessScoped(_corr_id),
                                   add_perfetto_annotations);
            tracing::pop_perfetto(category::rocm_memory_allocate{}, "", _track, _end_ts);
#    endif
        };
    };
}
#endif

postprocessing_callback
perfetto_post_processing::get_region_callback() const
{
    return [&]([[maybe_unused]] const storage_parsed_type_base& parsed) {
#if ROCPROFSYS_USE_ROCM > 0
        auto _rs = static_cast<const struct region_sample&>(parsed);

        auto _thrd_id  = _rs.thread_id;
        auto _corr_id  = _rs.correlation_id_internal;
        auto _beg_ts   = _rs.start_timestamp;
        auto _end_ts   = _rs.end_timestamp;
        auto _category = _rs.category;
        auto _name     = _rs.name;

        auto args = process_arguments_string(_rs.args_str);

        auto add_annotations = [&](::perfetto::EventContext ctx) {
            if(config::get_perfetto_annotations())
            {
                tracing::add_perfetto_annotation(ctx, "begin_ns", _beg_ts);
                tracing::add_perfetto_annotation(ctx, "corr_id", _corr_id);
                for(const auto& arg : args)
                    tracing::add_perfetto_annotation(ctx, arg.arg_name, arg.arg_value);

                if(!_rs.call_stack.empty())
                {
                    try
                    {
                        auto backtrace = nlohmann::json::parse(_rs.call_stack);
                        for(const auto& [key, val] : backtrace.items())
                        {
                            tracing::add_perfetto_annotation(ctx, key,
                                                             val.get<std::string>());
                        }
                    } catch(const std::exception& e)
                    {
                        ROCPROFSYS_VERBOSE_F(2, "Failed to parse call_stack JSON: %s\n",
                                             e.what());
                    }
                }
            }
        };

        tracing::push_perfetto_ts(category::rocm{}, _name.c_str(), _beg_ts,
                                  ::perfetto::Flow::ProcessScoped(_corr_id),
                                  add_annotations);
        tracing::pop_perfetto_ts(category::rocm{}, _name.c_str(), _end_ts);
#endif
    };
}

void
perfetto_post_processing::register_parser_callback(
    [[maybe_unused]] storage_parser& parser)
{
#if ROCPROFSYS_USE_ROCM > 0
    if(!get_use_perfetto())
    {
        return;
    }
    parser.register_type_callback(entry_type::region, get_region_callback());
    parser.register_type_callback(entry_type::kernel_dispatch,
                                  get_kernel_dispatch_callback());
    parser.register_type_callback(entry_type::memory_copy, get_memory_copy_callback());

    ROCPROFSYS_DEBUG("Buffer parser callbacks are registered..");
#endif
}

}  // namespace trace_cache
}  // namespace rocprofsys