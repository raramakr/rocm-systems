// MIT License
//
// Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

#pragma once

#include <rocprofiler-sdk/agent.h>
#include <rocprofiler-sdk/defines.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/hsa.h>

ROCPROFILER_EXTERN_C_INIT

/**
 * @brief (experimental) SPM parameter type used to configure SPM service.
 *
 **/
typedef enum ROCPROFILER_SDK_EXPERIMENTAL rocprofiler_spm_parameter_type_t
{
    ROCPROFILER_SPM_PARAMETER_TYPE_SAMPLE_FREQUENCY = 0,  ///< SPM sample frequency for Sclock
    ROCPROFILER_SPM_PARAMETER_TYPE_BUFFER_SIZE,           ///< SPM Buffer size, in bytes.
    ROCPROFILER_SPM_PARAMETER_TYPE_TIMEOUT_MS,  ///< SPM timeout in ms. Time to wait to read the SPM
                                                ///< buffer
    ROCPROFILER_SPM_PARAMETER_TYPE_LAST
} rocprofiler_spm_parameter_type_t;

/**
 * @brief (experimental) SPM parameter type and value.
 *
 **/
typedef struct ROCPROFILER_SDK_EXPERIMENTAL rocprofiler_spm_parameter_t
{
    rocprofiler_spm_parameter_type_t type;   ///< SPM Parameter type
    uint64_t                         value;  ///< SPM Parameter value
} rocprofiler_spm_parameter_t;

/**
 * @brief (experimental) SPM record flags.
 *
 **/
typedef enum ROCPROFILER_SDK_EXPERIMENTAL rocprofiler_spm_record_flag_t
{
    ROCPROFILER_SPM_RECORD_FLAG_DATA_LOST = 0,  ///< Set if callback has records with data loss
    ROCPROFILER_SPM_RECORD_FLAG_END,   ///< Set if callback is for end of dispatch record or end of
                                       ///< agent service
    ROCPROFILER_SPM_RECORD_FLAG_DATA,  ///< Set if callback has received data
    ROCPROFILER_SPM_RECORD_FLAG_LAST,
} rocprofiler_spm_record_flag_t;

/**
 * @brief (experimental) SPM record counter record.
 *
 **/
typedef struct ROCPROFILER_SDK_EXPERIMENTAL rocprofiler_spm_counter_record_t
{
    uint64_t               size;  ///< Size of this structure. Used for versioning and validation.
    rocprofiler_agent_id_t agent_id;              ///< Agent on which the record is collected
    rocprofiler_counter_instance_id_t id;         ///< Counter instance id
    rocprofiler_timestamp_t           timestamp;  ///< timestamp of the sample
    uint64_t value;  ///< SPM sample for the counter with counter instance id: id
} rocprofiler_spm_counter_record_t;

/**
 * @brief (experimental) Callback to receive SPM data
 *
 * @param [in] agent  agent that generated the spm data
 * @param [in] records pointer to the array of SPM records
 * @param [in] size_t  size of the record array
 * @param [in] userdata Passed back to user via dispatch_userdata or _configure_spm_agent_service
 */
typedef ROCPROFILER_SDK_EXPERIMENTAL void (*rocprofiler_spm_data_callback_t)(
    rocprofiler_spm_counter_record_t* records,
    size_t                            record_count,
    uint8_t                           flags,
    rocprofiler_user_data_t           userdata);
/**
 * @brief (experimental) Callback query if dispatch should be profiled
 *
 * @param [in] agent_id Which agent generated the spm data
 * @param [in] queue_id rocprofiler queue_id
 * @param [in] kernel_id kernel_id
 * @param [in] dispatch_id dispatch_id
 * @param [in] config_userdata Passed back from rocprofiler_configure_spm_dispatch_service
 * @param [out] dispatch_userdata To be passed in rocprofiler_spm_data_callback_t
 *
 * @retval 1 dispatch should be profiled
 * @retval 0 dispatch should be not profiled
 */
typedef ROCPROFILER_SDK_EXPERIMENTAL int (*rocprofiler_spm_dispatch_callback_t)(
    rocprofiler_agent_id_t    agent_id,
    rocprofiler_queue_id_t    queue_id,
    rocprofiler_kernel_id_t   kernel_id,
    rocprofiler_dispatch_id_t dispatch_id,
    void*                     config_userdata,
    rocprofiler_user_data_t*  dispatch_userdata);

ROCPROFILER_EXTERN_C_FINI
