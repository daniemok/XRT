/**
 * Copyright (C) 2020 Xilinx, Inc
 * Author(s): Larry Liu
 * ZNYQ XRT Library layered on top of ZYNQ zocl kernel driver
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

#include "aie.h"
#include "core/common/error.h"
#ifndef __AIESIM__
#include "core/common/message.h"
#include "core/edge/user/shim.h"
#include "xaiengine/xlnx-ai-engine.h"
#include "aie_event.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#include <iostream>
#include <cerrno>

namespace zynqaie {

XAie_InstDeclare(DevInst, &ConfigPtr);   // Declare global device instance

Aie::Aie(const std::shared_ptr<xrt_core::device>& device)
{
    XAie_SetupConfig(ConfigPtr, HW_GEN, XAIE_BASE_ADDR, XAIE_COL_SHIFT,
        XAIE_ROW_SHIFT, XAIE_NUM_COLS, XAIE_NUM_ROWS,
        XAIE_SHIM_ROW, XAIE_RESERVED_TILE_ROW_START,
        XAIE_RESERVED_TILE_NUM_ROWS, XAIE_AIE_TILE_ROW_START,
        XAIE_AIE_TILE_NUM_ROWS);

#ifndef __AIESIM__
    auto drv = ZYNQ::shim::handleCheck(device->get_device_handle());

    /* TODO get partition id and uid from XCLBIN or PDI */
    uint32_t partition_id = 1;
    uint32_t uid = 0;
    drm_zocl_aie_fd aiefd = { partition_id, uid, 0 };
    int ret = drv->getPartitionFd(aiefd);
    if (ret)
        throw xrt_core::error(ret, "Create AIE failed. Can not get AIE fd");
    fd = aiefd.fd;

    ConfigPtr.PartProp.Handle = fd;
#endif

    AieRC rc;
    if ((rc = XAie_CfgInitialize(&DevInst, &ConfigPtr)) != XAIE_OK)
        throw xrt_core::error(-EINVAL, "Failed to initialize AIE configuration: " + std::to_string(rc));
    devInst = &DevInst;

    /* Initialize PLIO metadata */
    for (auto& plio : xrt_core::edge::aie::get_plios(device.get()))
        plios.emplace_back(std::move(plio));

    /* Initialize graph GMIO metadata */
    for (auto& gmio : xrt_core::edge::aie::get_gmios(device.get()))
        gmios.emplace_back(std::move(gmio));

    /*
     * Initialize AIE shim DMA on column base if there is one for
     * this column.
     */
    numCols = XAIE_NUM_COLS;
    shim_dma.resize(numCols);
    for (auto& gmio : gmios) {
        if (gmio.shim_col > numCols)
            throw xrt_core::error(-EINVAL, "GMIO " + gmio.name + " shim column " + std::to_string(gmio.shim_col) + " does not exist");

        auto dma = &shim_dma.at(gmio.shim_col);
        XAie_LocType shimTile = XAie_TileLoc(gmio.shim_col, 0);

        if (!dma->configured) {
            XAie_DmaDescInit(devInst, &(dma->desc), shimTile);
            dma->configured = true;
        }

        auto chan = gmio.channel_number;
        /* type 0: GM->AIE; type 1: AIE->GM */
        XAie_DmaDirection dir = gmio.type == 0 ? DMA_MM2S : DMA_S2MM;
        uint8_t pch = CONVERT_LCHANL_TO_PCHANL(chan);
        XAie_DmaChannelEnable(devInst, shimTile, pch, dir);
        XAie_DmaSetAxi(&(dma->desc), 0, gmio.burst_len, 0, 0, 0);

        XAie_DmaGetMaxQueueSize(devInst, shimTile, &(dma->maxqSize));
        for (int i = 0; i < dma->maxqSize; ++i) {
            /*
             * 16 BDs are allocated to 4 channels.
             * Channel0: BD0~BD3
             * Channel1: BD4~BD7
             * Channel2: BD8~BD11
             * Channel3: BD12~BD15
             */
            int bd_num = chan * dma->maxqSize + i;
            BD bd;
            bd.bd_num = bd_num;
            dma->dma_chan[chan].idle_bds.push(bd);
        }
    }

    Resources::AIE::initialize(XAIE_NUM_COLS, XAIE_NUM_ROWS);
}

Aie::~Aie()
{
#ifndef __AIESIM__
  if (devInst)
    XAie_Finish(devInst);
#endif
}

XAie_DevInst* Aie::getDevInst()
{
  if (!devInst)
    throw xrt_core::error(-EINVAL, "AIE is not initialized");

  return devInst;
}

void
Aie::
sync_bo(xrtBufferHandle bo, const char *gmioName, enum xclBOSyncDirection dir, size_t size, size_t offset)
{
  if (!devInst)
    throw xrt_core::error(-EINVAL, "Can't sync BO: AIE is not initialized");

  auto gmio = std::find_if(gmios.begin(), gmios.end(),
            [gmioName](gmio_type it) { return it.name.compare(gmioName) == 0; });

  if (gmio == gmios.end())
    throw xrt_core::error(-EINVAL, "Can't sync BO: GMIO name not found");

  submit_sync_bo(bo, gmio, dir, size, offset);

  ShimDMA *dmap = &shim_dma.at(gmio->shim_col);
  auto chan = gmio->channel_number;
  auto shim_tile = XAie_TileLoc(gmio->shim_col, 0);
  XAie_DmaDirection gmdir = gmio->type == 0 ? DMA_MM2S : DMA_S2MM;

  wait_sync_bo(dmap, chan, shim_tile, gmdir, 0);
}

void
Aie::
sync_bo_nb(xrtBufferHandle bo, const char *gmioName, enum xclBOSyncDirection dir, size_t size, size_t offset)
{
  if (!devInst)
    throw xrt_core::error(-EINVAL, "Can't sync BO: AIE is not initialized");

  auto gmio = std::find_if(gmios.begin(), gmios.end(),
            [gmioName](gmio_type it) { return it.name.compare(gmioName) == 0; });

  if (gmio == gmios.end())
    throw xrt_core::error(-EINVAL, "Can't sync BO: GMIO name not found");

  submit_sync_bo(bo, gmio, dir, size, offset);
}

void
Aie::
wait_gmio(const std::string& gmioName)
{
  if (!devInst)
    throw xrt_core::error(-EINVAL, "Can't wait GMIO: AIE is not initialized");

  auto gmio = std::find_if(gmios.begin(), gmios.end(),
            [gmioName](gmio_type it) { return it.name.compare(gmioName) == 0; });

  if (gmio == gmios.end())
    throw xrt_core::error(-EINVAL, "Can't wait GMIO: GMIO name not found");

  ShimDMA *dmap = &shim_dma.at(gmio->shim_col);
  auto chan = gmio->channel_number;
  auto shim_tile = XAie_TileLoc(gmio->shim_col, 0);
  XAie_DmaDirection gmdir = gmio->type == 0 ? DMA_MM2S : DMA_S2MM;

  wait_sync_bo(dmap, chan, shim_tile, gmdir, 0);
}

void
Aie::
submit_sync_bo(xrtBufferHandle bo, std::vector<gmio_type>::iterator& gmio, enum xclBOSyncDirection dir, size_t size, size_t offset)
{
  switch (dir) {
  case XCL_BO_SYNC_BO_GMIO_TO_AIE:
    if (gmio->type != 0)
      throw xrt_core::error(-EINVAL, "Sync BO direction does not match GMIO type");
    break;
  case XCL_BO_SYNC_BO_AIE_TO_GMIO:
    if (gmio->type != 1)
      throw xrt_core::error(-EINVAL, "Sync BO direction does not match GMIO type");
    break;
  default:
    throw xrt_core::error(-EINVAL, "Can't sync BO: unknown direction.");
  }

  if (size & XAIEDMA_SHIM_TXFER_LEN32_MASK != 0)
    throw xrt_core::error(-EINVAL, "Sync AIE Bo fails: size is not 32 bits aligned.");

  ShimDMA *dmap = &shim_dma.at(gmio->shim_col);
  auto chan = gmio->channel_number;
  auto shim_tile = XAie_TileLoc(gmio->shim_col, 0);
  XAie_DmaDirection gmdir = gmio->type == 0 ? DMA_MM2S : DMA_S2MM;
  uint32_t pchan = CONVERT_LCHANL_TO_PCHANL(chan);

  /* Find a free BD. Busy wait until we get one. */
  while (dmap->dma_chan[chan].idle_bds.empty()) {
    uint8_t npend;
    XAie_DmaGetPendingBdCount(devInst, shim_tile, pchan, gmdir, &npend);

    int num_comp = dmap->maxqSize - npend;

    /* Pending BD is completed by order per Shim DMA spec. */
    for (int i = 0; i < num_comp; ++i) {
      BD bd = dmap->dma_chan[chan].pend_bds.front();
      clear_bd(bd);
      dmap->dma_chan[chan].pend_bds.pop();
      dmap->dma_chan[chan].idle_bds.push(bd);
    }
  }

  BD bd = dmap->dma_chan[chan].idle_bds.front();
  dmap->dma_chan[chan].idle_bds.pop();
  prepare_bd(bd, bo);
#ifndef __AIESIM__
  XAie_DmaSetAddrLen(&(dmap->desc), (uint64_t)(bd.vaddr + offset), size);
#else
  XAie_DmaSetAddrLen(&(dmap->desc), (uint64_t)(xrtBOAddress(bo) + offset), size);
#endif

  /* Set BD lock */
  auto acq_lock = XAie_LockInit(bd.bd_num, XAIE_LOCK_WITH_NO_VALUE);
  auto rel_lock = XAie_LockInit(bd.bd_num, XAIE_LOCK_WITH_NO_VALUE);
  XAie_DmaSetLock(&(dmap->desc), acq_lock, rel_lock);

  XAie_DmaEnableBd(&(dmap->desc));

  /* Write BD */
  XAie_DmaWriteBd(devInst, &(dmap->desc), shim_tile, bd.bd_num);

  /* Enqueue BD */
  XAie_DmaChannelPushBdToQueue(devInst, shim_tile, pchan, gmdir, bd.bd_num);
  dmap->dma_chan[chan].pend_bds.push(bd);
}

void
Aie::
wait_sync_bo(ShimDMA *dmap, uint32_t chan, XAie_LocType& tile, XAie_DmaDirection gmdir, uint32_t timeout)
{
  while (XAie_DmaWaitForDone(devInst, tile, CONVERT_LCHANL_TO_PCHANL(chan), gmdir, timeout) != XAIE_OK);

  while (!dmap->dma_chan[chan].pend_bds.empty()) {
    BD bd = dmap->dma_chan[chan].pend_bds.front();
    clear_bd(bd);
    dmap->dma_chan[chan].pend_bds.pop();
    dmap->dma_chan[chan].idle_bds.push(bd);
  }
}

void
Aie::
prepare_bd(BD& bd, xrtBufferHandle& bo)
{
#ifndef __AIESIM__
  auto buf_fd = xrtBOExport(bo);
  if (buf_fd == XRT_NULL_BO_EXPORT)
    throw xrt_core::error(-errno, "Sync AIE Bo: fail to export BO.");
  bd.buf_fd = buf_fd;

  auto ret = ioctl(fd, AIE_ATTACH_DMABUF_IOCTL, buf_fd);
  if (ret)
    throw xrt_core::error(-errno, "Sync AIE Bo: fail to attach DMA buf.");

  auto bosize = xrtBOSize(bo);
  bd.size = bosize;

  bd.vaddr = reinterpret_cast<char *>(mmap(NULL, bosize, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0));
#endif
}

void
Aie::
clear_bd(BD& bd)
{
#ifndef __AIESIM__
  munmap(bd.vaddr, bd.size);
  bd.vaddr = nullptr;
  auto ret = ioctl(fd, AIE_DETACH_DMABUF_IOCTL, bd.buf_fd);
  if (ret)
    throw xrt_core::error(-errno, "Sync AIE Bo: fail to detach DMA buf.");
#endif
}

void
Aie::
reset(const xrt_core::device* device)
{
#ifndef __AIESIM__
    if (!devInst)
        throw xrt_core::error(-EINVAL, "Can't Reset AIE: AIE is not initialized");

    XAie_Finish(devInst);
    devInst = nullptr;

    auto drv = ZYNQ::shim::handleCheck(device->get_device_handle());

    /* TODO get partition id and uid from XCLBIN or PDI */
    uint32_t partition_id = 1;

    drm_zocl_aie_reset reset = { partition_id };
    int ret = drv->resetAIEArray(reset);
    if (ret)
        throw xrt_core::error(ret, "Fail to reset AIE Array");
#endif
}

int
Aie::
start_profiling(int option, const std::string& port1_name, const std::string& port2_name, uint32_t value)
{
  int handle = -1;

  if (!devInst)
    throw xrt_core::error(-EINVAL, "Start profiling fails: AIE is not initialized");

  if (option != IO_STREAM_RUNNING_EVENT_COUNT)
    throw xrt_core::error(-EINVAL, "Start profiling fails: unknown profiling option.");

  auto gmio = std::find_if(gmios.begin(), gmios.end(),
            [&port1_name](auto& it) { return it.name.compare(port1_name) == 0; });

  // For PLIO inside graph, there is no name property.
  // So we need to match logical name too
  auto plio = std::find_if(plios.begin(), plios.end(),
            [&port1_name](auto& it) { return it.name.compare(port1_name) == 0; });
  if (plio == plios.end()) {
    plio = std::find_if(plios.begin(), plios.end(),
            [&port1_name](auto& it) { return it.logical_name.compare(port1_name) == 0; });
  }

  if (gmio == gmios.end() && plio == plios.end())
    throw xrt_core::error(-EINVAL, "Can't start profiling: port name '" + port1_name + "' not found");

  if (gmio != gmios.end() && plio != plios.end())
    throw xrt_core::error(-EINVAL, "Can't start profiling: ambiguous port name '" + port1_name + "'");

  XAie_LocType shim_tile;
  XAie_StrmPortIntf mode;
  uint8_t stream_id;
  if (gmio != gmios.end()) {
    shim_tile = XAie_TileLoc(gmio->shim_col, 0);
    /* type 0: GM->AIE; type 1: AIE->GM */
    mode = gmio->type == 1 ? XAIE_STRMSW_MASTER : XAIE_STRMSW_SLAVE;
    stream_id = gmio->stream_id;
  } else {
    shim_tile = XAie_TileLoc(plio->shim_col, 0);
    mode = plio->is_master ? XAIE_STRMSW_MASTER: XAIE_STRMSW_SLAVE;
    stream_id = plio->stream_id;
  }

  int handleId = eventRecords.size();
  int eventPortId = Resources::AIE::getShimTile(shim_tile.Col)->plModule.requestStreamEventPort(handleId);
  int counterId = Resources::AIE::getShimTile(shim_tile.Col)->plModule.requestPerformanceCounter(handleId);
  if (counterId >= 0 && eventPortId >= 0) {
    XAie_EventSelectStrmPort(devInst, shim_tile, (uint8_t)eventPortId, mode, SOUTH, stream_id);
    XAie_PerfCounterControlSet(devInst, shim_tile, XAIE_PL_MOD, (uint8_t)counterId, XAIETILE_EVENT_SHIM_PORT_RUNNING[eventPortId],                                     XAIETILE_EVENT_SHIM_PORT_RUNNING[eventPortId]);
    eventRecords.push_back({ option,
                { { shim_tile, Resources::pl_module, Resources::performance_counter, (size_t)counterId },
                { shim_tile, Resources::pl_module, Resources::stream_switch_event_port, (size_t)eventPortId } } });
    handle = handleId;
  } else {
    if (counterId >= 0)
      Resources::AIE::getShimTile(shim_tile.Col)->plModule.releasePerformanceCounter(handleId, counterId);
    if (eventPortId >= 0)
      Resources::AIE::getShimTile(shim_tile.Col)->plModule.releaseStreamEventPort(handleId, eventPortId);
    throw xrt_core::error(-EAGAIN, "Can't start profiling: Failed to request performance counter or stream switch event port resources.");
  }

  return handle;
}

uint64_t
Aie::
read_profiling(int phdl)
{
  uint64_t value = 0;

  std::vector<Resources::AcquiredResource>& acquiredResourcesForThisHandle = eventRecords[phdl].acquiredResources;

  Resources::AcquiredResource& acquiredResource = acquiredResourcesForThisHandle[0];
  XAie_ModuleType XAieModuleType = AIEResourceModuletoXAieModuleTypeMap[acquiredResource.module];

  if (acquiredResource.resource == Resources::performance_counter)
    XAie_PerfCounterGet(devInst, acquiredResource.loc, XAieModuleType, acquiredResource.id, (u32*)(&value));
  else
    throw xrt_core::error(-EAGAIN, "Can't read profiling: The acquired resources order does not match the profiling option.");

  return value;
}

void
Aie::
stop_profiling(int phdl)
{
  if (phdl < eventRecords.size() && eventRecords[phdl].option >= 0) {
    std::vector<Resources::AcquiredResource>& acquiredResourcesForThisHandle = eventRecords[phdl].acquiredResources;
    for (int i = 0; i < acquiredResourcesForThisHandle.size(); i++) {
      Resources::AcquiredResource& acquiredResource = acquiredResourcesForThisHandle[i];
      XAie_ModuleType XAieModuleType = AIEResourceModuletoXAieModuleTypeMap[acquiredResource.module];

      if (acquiredResource.resource == Resources::performance_counter) {
        u8 counterId = acquiredResource.id;

        XAie_PerfCounterReset(devInst, acquiredResource.loc, XAieModuleType, counterId);
        XAie_PerfCounterResetControlReset(devInst, acquiredResource.loc, XAieModuleType, counterId);

        if (acquiredResource.module == Resources::pl_module)
          Resources::AIE::getShimTile(acquiredResource.loc.Col)->plModule.releasePerformanceCounter(phdl, counterId);
        else if (acquiredResource.module == Resources::core_module)
          Resources::AIE::getAIETile(acquiredResource.loc.Col, acquiredResource.loc.Row - 1)->coreModule.releasePerformanceCounter(phdl, counterId);
      } else if (acquiredResource.resource == Resources::stream_switch_event_port) {
        u8 eventPortId = acquiredResource.id;

        XAie_EventSelectStrmPortReset(devInst, acquiredResource.loc, eventPortId);

        if (acquiredResource.module == Resources::pl_module)
          Resources::AIE::getShimTile(acquiredResource.loc.Col)->plModule.releaseStreamEventPort(phdl, eventPortId);
      }
    }
  }
}

}
