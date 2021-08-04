/**
 * Copyright (C) 2016-2018 Xilinx, Inc
 * Author: Hem C Neema
 * Simple command line utility to inetract with SDX PCIe devices
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#define XRT_CORE_COMMON_SOURCE // in same dll as core_common
#include "utils.h"
#include "system.h"
#include "device.h"
#include "query_requests.h"
#include <string>
#include <atomic>
#include <cstdint>
#include <limits>
#include <boost/algorithm/string.hpp>

namespace {

inline unsigned int
bit(unsigned int lsh)
{
  return (0x1 << lsh);
}

static std::string
precision(double value, int p)
{
  std::stringstream stream;
  stream << std::fixed << std::setprecision(p) << value;
  return stream.str();
}



}

namespace xrt_core { namespace utils {

std::string
parse_cu_status(unsigned int val)
{
  char delim = '(';
  std::string status;
  if (val == std::numeric_limits<uint32_t>::max()) //Crashed soft kernel status is -1
    status = "(CRASHED)";
  else if (val == 0x0)
    status = "(--)";
  else {
    if (val & CU_AP_START) {
      status += delim;
      status += "START";
      delim = '|';
    }
    if (val & CU_AP_DONE) {
      status += delim;
      status += "DONE";
      delim = '|';
    }
    if (val & CU_AP_IDLE) {
      status += delim;
      status += "IDLE";
      delim = '|';
    }
    if (val & CU_AP_READY) {
      status += delim;
      status += "READY";
      delim = '|';
    }
    if (val & CU_AP_CONTINUE) {
      status += delim;
      status += "RESTART";
      delim = '|';
    }
    if (status.size())
      status += ')';
    else 
      status = "(UNKNOWN)";
  }
  return status;
}

std::string
parse_firewall_status(unsigned int val)
{
  char delim = '(';
  std::string status;
  // Read channel error
  if (val & bit(0)) {
    status += delim;
    status += "READ_RESPONSE_BUSY";
    delim = '|';
  }
  if (val & bit(1)) {
    status += delim;
    status += "RECS_ARREADY_MAX_WAIT";
    delim = '|';
  }
  if (val & bit(2)) {
    status += delim;
    status += "RECS_CONTINUOUS_RTRANSFERS_MAX_WAIT";
    delim = '|';
  }
  if (val & bit(3)) {
    status += delim;
    status += "ERRS_RDATA_NUM";
    delim = '|';
  }
  if (val & bit(4)) {
    status += delim;
    status += "ERRS_RID";
    delim = '|';
  }
  // Write channel error
  if (val & bit(16)) {
    status += delim;
    status += "WRITE_RESPONSE_BUSY";
    delim = '|';
  }
  if (val & bit(17)) {
    status += delim;
    status += "RECS_AWREADY_MAX_WAIT";
    delim = '|';
  }
  if (val & bit(18)) {
    status += delim;
    status += "RECS_WREADY_MAX_WAIT";
    delim = '|';
  }
  if (val & bit(19)) {
    status += delim;
    status += "RECS_WRITE_TO_BVALID_MAX_WAIT";
    delim = '|';
  }
  if (val & bit(20)) {
    status += delim;
    status += "ERRS_BRESP";
    delim = '|';
  }
  if (status.size())
    status += ')';
  else if (val == 0x0)
    status = "(GOOD)";
  else
    status = "(UNKNOWN)";
  return status;
}

std::string
parse_dna_status(unsigned int val)
{
  char delim = '(';
  std::string status;
  if (val & bit(0)) {
    status += delim;
    status += "PASS";
    delim = '|';
  }
  else{
    status += delim;
    status += "FAIL";
    delim = '|';
  }
  if (status.size())
    status += ')';
  else
    status = "(UNKNOWN)";
  return status;
}

std::string
unit_convert(size_t size)
{
  int i = 0, bit_shift=6;
  std::string ret, unit[8]={"Byte", "KB", "MB", "GB", "TB", "PB", "EB", "ZB"};
  if(size < 64)
    return std::to_string(size)+" "+unit[i];
  if(!(size & (size-1)))
    bit_shift = 0;
  while( (size>>bit_shift) !=0 && i<8){
    ret = std::to_string(size);
    size >>= 10;
    i++;
  }
  return ret+" "+unit[i-1];
}

std::string
format_base10_shiftdown3(uint64_t value)
{
  constexpr double decimal_shift = 1000.0;
  constexpr int digit_precision = 3;
  return precision(static_cast<double>(value) / decimal_shift, digit_precision);
}

std::string
format_base10_shiftdown6(uint64_t value)
{
  constexpr double decimal_shift = 1000000.0;
  constexpr int digit_precision = 6;
  return precision(static_cast<double>(value) / decimal_shift, digit_precision);
}

uint64_t
issue_id()
{
  static std::atomic<uint64_t> id {0} ;
  return id++;
}

static const std::map<std::string, std::string> clock_map = {
  {"DATA_CLK", "Data"},
  {"KERNEL_CLK", "Kernel"},
  {"SYSTEM_CLK", "System"},
};

std::string 
parse_clock_id(const std::string& id)
{
  auto clock_str = clock_map.find(id);
  return clock_str != clock_map.end() ? clock_str->second : "N/A";
}

}} // utils, xrt_core
