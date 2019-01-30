/* virtual_mmc.c
 *
 * Copyright (C) 2017 Motoharu Gosuto
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "virtual_mmc.h"

#include <psp2kern/kernel/threadmgr.h>

#include <string.h>

#include <taihen.h>

#include "hook_ids.h"
#include "functions.h"
#include "reader.h"
#include "global_log.h"
#include "cmd56_key.h"
#include "sector_api.h"
#include "mmc_emu.h"
#include "ins_rem_card.h"
#include "media_id_emu.h"
#include "defines.h"

//redirect read operations to separate thread
int mmc_read_hook_threaded(void* ctx_part, int sector,	char* buffer, int nSectors)
{
  //make sure that only mmc operations are redirected
  if(ksceSdifGetSdContextGlobal(SCE_SDIF_DEV_GAME_CARD) == ((sd_context_part_base*)ctx_part)->gctx_ptr)
  {
    //check if media-id read is requested
    int media_id_res = read_media_id(get_mbr_ptr(), sector, buffer, nSectors);
    if(media_id_res > 0)
      return 0;

    g_ctx_part = ctx_part;
    g_sector = sector;
    g_buffer = buffer;
    g_nSectors = nSectors;

    //send request
    ksceKernelSignalCond(req_cond);

    //lock mutex
    int res = ksceKernelLockMutex(resp_lock, 1, 0);
    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
    {
      snprintf(sprintfBuffer, 256, "failed to ksceKernelLockMutex resp_lock : %x\n", res);
      FILE_GLOBAL_WRITE_LEN(sprintfBuffer);
    }
    #endif

    //wait for response
    res = ksceKernelWaitCond(resp_cond, 0);
    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
    {
      snprintf(sprintfBuffer, 256, "failed to ksceKernelWaitCond resp_cond : %x\n", res);
      FILE_GLOBAL_WRITE_LEN(sprintfBuffer);
    }
    #endif

    //unlock mutex
    res = ksceKernelUnlockMutex(resp_lock, 1);
    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
    {
      snprintf(sprintfBuffer, 256, "failed to ksceKernelUnlockMutex resp_lock : %x\n", res);
      FILE_GLOBAL_WRITE_LEN(sprintfBuffer);
    }
    #endif

    return g_res;
  }
  else
  {
    int res = TAI_CONTINUE(int, mmc_read_hook_ref, ctx_part, sector, buffer, nSectors);
    return res;
  }
}

int mmc_write_hook(void* ctx_part, int sector, char* buffer, int nSectors)
{
  //make sure that only mmc operations are redirected
  if(ksceSdifGetSdContextGlobal(SCE_SDIF_DEV_GAME_CARD) == ((sd_context_part_base*)ctx_part)->gctx_ptr)
  {
    int media_id_res = write_media_id(get_mbr_ptr(), sector, buffer, nSectors);
    if(media_id_res > 0)
      return 0;

    #ifdef ENABLE_DEBUG_LOG
    FILE_GLOBAL_WRITE_LEN("Write operation is not supported\n");
    #endif

    memset(buffer, 0, nSectors * SD_DEFAULT_SECTOR_SIZE);
    return SD_UNKNOWN_READ_WRITE_ERROR;
  }
  else
  {
    int res = TAI_CONTINUE(int, mmc_write_hook_ref, ctx_part, sector, buffer, nSectors);
    return res;
  }
}

//emulate mmc command
int send_command_emu_hook(sd_context_global* ctx, cmd_input* cmd_data1, cmd_input* cmd_data2, int nIter, int num)
{
  if(ksceSdifGetSdContextGlobal(SCE_SDIF_DEV_GAME_CARD) == ctx)
  {
    //can add debug code here

    int res = emulate_mmc_command(ctx, cmd_data1, cmd_data2, nIter, num);

    //can add debug code here

    return res;
  }
  else
  {
    int res = TAI_CONTINUE(int, send_command_hook_ref, ctx, cmd_data1, cmd_data2, nIter, num);
    return res;
  }
}

//override cmd56 handshake with dumped keys
//this function is called only for game cards in mmc mode
int gc_cmd56_handshake_override_hook(int param0)
{
  //get data from iso
  char data_5018_buffer[CMD56_DATA_SIZE];
  get_cmd56_data(data_5018_buffer);

  //set data in gc memory
  set_5018_data(data_5018_buffer);

  #ifdef ENABLE_DEBUG_LOG
  FILE_GLOBAL_WRITE_LEN("override cmd56 handshake\n");
  #endif

  return 0;
}

int initialize_hooks_virtual_mmc()
{
  tai_module_info_t sdstor_info;
  sdstor_info.size = sizeof(tai_module_info_t);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceSdstor", &sdstor_info) >= 0)
  {
    //override cmd56 handshake with dumped keys
    gc_cmd56_handshake_hook_id = taiHookFunctionImportForKernel(KERNEL_PID, &gc_cmd56_handshake_hook_ref, "SceSdstor", SceSblGcAuthMgrGcAuthForDriver_NID, 0x68781760, gc_cmd56_handshake_override_hook);

    #ifdef ENABLE_DEBUG_LOG
    if(gc_cmd56_handshake_hook_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init gc_cmd56_handshake_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init gc_cmd56_handshake_hook\n");
    #endif

    //redirect read operations to separate thread
    mmc_read_hook_id = taiHookFunctionImportForKernel(KERNEL_PID, &mmc_read_hook_ref, "SceSdstor", SceSdifForDriver_NID, 0x6f8d529b, mmc_read_hook_threaded);

    #ifdef ENABLE_DEBUG_LOG
    if(mmc_read_hook_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init mmc_read_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init mmc_read_hook\n");
    #endif

    mmc_write_hook_id = taiHookFunctionImportForKernel(KERNEL_PID, &mmc_write_hook_ref, "SceSdstor", SceSdifForDriver_NID, 0x175543d2, mmc_write_hook);

    #ifdef ENABLE_DEBUG_LOG
    if(mmc_write_hook_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init mmc_write_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init mmc_write_hook\n");
    #endif

    char zeroCallOnePatch[4] = {0x01, 0x20, 0x00, 0xBF};

    //this patch enables card CID check instead of default
    //gcd-lp-act-mediaid check with requires MBR and raw partition
    //in case of virtual mode it also requires emulation of write operations
    //WARNING: this patch partially fixes suspend/resume problem but there is still some glitch

    //suspend_cid_check_patch_id = taiInjectDataForKernel(KERNEL_PID, sdstor_info.modid, 0, 0x4A1C, zeroCallOnePatch, 4); //patch (BLX) to (MOVS R0, #1 ; NOP)

    #ifdef ENABLE_DEBUG_LOG
    if(suspend_cid_check_patch_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init suspend_cid_check_patch\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init suspend_cid_check_patch\n");
    #endif

    //this patch enables card CID check instead of default
    //gcd-lp-act-mediaid check with requires MBR and raw partition
    //in case of virtual mode it also requires emulation of write operations
    //WARNING: this patch partially fixes suspend/resume problem but there is still some glitch

    //resume_cid_check_patch_id =  taiInjectDataForKernel(KERNEL_PID, sdstor_info.modid, 0, 0x4B6E, zeroCallOnePatch, 4); //patch (BLX) to (MOVS R0, #1 ; NOP)

    #ifdef ENABLE_DEBUG_LOG
    if(resume_cid_check_patch_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init resume_cid_check_patch\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init resume_cid_check_patch\n");
    #endif
  }

  tai_module_info_t sdif_info;
  sdif_info.size = sizeof(tai_module_info_t);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceSdif", &sdif_info) >= 0)
  {
    send_command_hook_id = taiHookFunctionOffsetForKernel(KERNEL_PID, &send_command_hook_ref, sdif_info.modid, 0, 0x17E8, 1, send_command_emu_hook);

    #ifdef ENABLE_DEBUG_LOG
    if(send_command_hook_id < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to init send_command_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Init send_command_hook\n");
    #endif
  }

  initialize_ins_rem();
  init_media_id_emu();

  return 0;
}

int deinitialize_hooks_virtual_mmc()
{
  if(gc_cmd56_handshake_hook_id >= 0)
  {
    int res = taiHookReleaseForKernel(gc_cmd56_handshake_hook_id, gc_cmd56_handshake_hook_ref);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit gc_cmd56_handshake_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit gc_cmd56_handshake_hook\n");
    #endif

    gc_cmd56_handshake_hook_id = -1;
  }

  if(mmc_read_hook_id >= 0)
  {
    int res = taiHookReleaseForKernel(mmc_read_hook_id, mmc_read_hook_ref);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit mmc_read_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit mmc_read_hook\n");
    #endif

    mmc_read_hook_id = -1;
  }

  if(mmc_write_hook_id >= 0)
  {
    int res = taiHookReleaseForKernel(mmc_write_hook_id, mmc_write_hook_ref);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit mmc_write_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit mmc_write_hook\n");
    #endif

    mmc_write_hook_id = -1;
  }

  if(suspend_cid_check_patch_id >= 0)
  {
    int res = taiInjectReleaseForKernel(suspend_cid_check_patch_id);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit suspend_cid_check_patch\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit suspend_cid_check_patch\n");
    #endif

    suspend_cid_check_patch_id = -1;
  }

  if(resume_cid_check_patch_id >= 0)
  {
    int res = taiInjectReleaseForKernel(resume_cid_check_patch_id);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit resume_cid_check_patch\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit resume_cid_check_patch\n");
    #endif

    resume_cid_check_patch_id = -1;
  }

  if(send_command_hook_id >= 0)
  {
    int res = taiHookReleaseForKernel(send_command_hook_id, send_command_hook_ref);

    #ifdef ENABLE_DEBUG_LOG
    if(res < 0)
      FILE_GLOBAL_WRITE_LEN("Failed to deinit send_command_hook\n");
    else
      FILE_GLOBAL_WRITE_LEN("Deinit send_command_hook\n");
    #endif

    send_command_hook_id = -1;
  }

  deinitialize_ins_rem();
  deinit_media_id_emu();

  return 0;
}
