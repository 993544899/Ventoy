/******************************************************************************
 * Ventoy.c
 *
 * Copyright (c) 2020, longpanda <admin@ventoy.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/RamDisk.h>
#include <Protocol/SimpleFileSystem.h>
#include <Ventoy.h>

UINTN g_iso_buf_size = 0;
BOOLEAN gMemdiskMode = FALSE;

ventoy_sector_flag *g_sector_flag = NULL;
UINT32 g_sector_flag_num = 0;

EFI_FILE_OPEN g_original_fopen = NULL;
EFI_FILE_CLOSE g_original_fclose = NULL;
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME g_original_open_volume = NULL;

/* EFI block device vendor device path GUID */
EFI_GUID gVtoyBlockDevicePathGuid = VTOY_BLOCK_DEVICE_PATH_GUID;

#define VENTOY_ISO9660_SECTOR_OVERFLOW  2097152

BOOLEAN g_fixup_iso9660_secover_enable = FALSE;
BOOLEAN g_fixup_iso9660_secover_start  = FALSE;
UINT64  g_fixup_iso9660_secover_1st_secs = 0;
UINT64  g_fixup_iso9660_secover_cur_secs = 0;
UINT64  g_fixup_iso9660_secover_tot_secs = 0;

EFI_STATUS EFIAPI ventoy_block_io_reset 
(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN BOOLEAN                        ExtendedVerification
) 
{
    (VOID)This;
    (VOID)ExtendedVerification;
	return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ventoy_read_iso_sector
(
    IN UINT64                 Sector,
    IN UINTN                  Count,
    OUT VOID                 *Buffer
)
{
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_LBA MapLba = 0;
    UINT32 i = 0;
    UINTN secLeft = 0;
    UINTN secRead = 0;
    UINT64 ReadStart = 0;
    UINT64 ReadEnd = 0;
    UINT64 OverrideStart = 0;
    UINT64 OverrideEnd= 0;
    UINT8 *pCurBuf = (UINT8 *)Buffer;
    ventoy_img_chunk *pchunk = g_chunk;
    ventoy_override_chunk *pOverride = g_override_chunk;
    EFI_BLOCK_IO_PROTOCOL *pRawBlockIo = gBlockData.pRawBlockIo;
    
    debug("read iso sector %lu  count %u", Sector, Count);

    ReadStart = Sector * 2048;
    ReadEnd = (Sector + Count) * 2048;

    for (i = 0; Count > 0 && i < g_img_chunk_num; i++, pchunk++)
    {
        if (Sector >= pchunk->img_start_sector && Sector <= pchunk->img_end_sector)
        {
            if (g_chain->disk_sector_size == 512)
            {
                MapLba = (Sector - pchunk->img_start_sector) * 4 + pchunk->disk_start_sector;
            }
            else
            {
                MapLba = (Sector - pchunk->img_start_sector) * 2048 / g_chain->disk_sector_size + pchunk->disk_start_sector;
            }

            secLeft = pchunk->img_end_sector + 1 - Sector;
            secRead = (Count < secLeft) ? Count : secLeft;

            Status = pRawBlockIo->ReadBlocks(pRawBlockIo, pRawBlockIo->Media->MediaId,
                                     MapLba, secRead * 2048, pCurBuf);
            if (EFI_ERROR(Status))
            {
                debug("Raw disk read block failed %r", Status);
                return Status;
            }

            Count -= secRead;
            Sector += secRead;
            pCurBuf += secRead * 2048;
        }
    }

    if (ReadStart > g_chain->real_img_size_in_bytes)
    {
        return EFI_SUCCESS;
    }

    /* override data */
    pCurBuf = (UINT8 *)Buffer;
    for (i = 0; i < g_override_chunk_num; i++, pOverride++)
    {
        OverrideStart = pOverride->img_offset;
        OverrideEnd = pOverride->img_offset + pOverride->override_size;
    
        if (OverrideStart >= ReadEnd || ReadStart >= OverrideEnd)
        {
            continue;
        }

        if (ReadStart <= OverrideStart)
        {
            if (ReadEnd <= OverrideEnd)
            {
                CopyMem(pCurBuf + OverrideStart - ReadStart, pOverride->override_data, ReadEnd - OverrideStart);  
            }
            else
            {
                CopyMem(pCurBuf + OverrideStart - ReadStart, pOverride->override_data, pOverride->override_size);
            }
        }
        else
        {
            if (ReadEnd <= OverrideEnd)
            {
                CopyMem(pCurBuf, pOverride->override_data + ReadStart - OverrideStart, ReadEnd - ReadStart); 
            }
            else
            {
                CopyMem(pCurBuf, pOverride->override_data + ReadStart - OverrideStart, OverrideEnd - ReadStart);
            }
        }

        if (g_fixup_iso9660_secover_enable && (!g_fixup_iso9660_secover_start) && 
            pOverride->override_size == sizeof(ventoy_iso9660_override))
        {
            ventoy_iso9660_override *dirent = (ventoy_iso9660_override *)pOverride->override_data;
            if (dirent->first_sector >= VENTOY_ISO9660_SECTOR_OVERFLOW)
            {
                g_fixup_iso9660_secover_start = TRUE;
                g_fixup_iso9660_secover_cur_secs = 0;
            }
        }
    }

    return EFI_SUCCESS;    
}

EFI_STATUS EFIAPI ventoy_block_io_ramdisk_read 
(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                          MediaId,
    IN EFI_LBA                         Lba,
    IN UINTN                           BufferSize,
    OUT VOID                          *Buffer
) 
{
    //debug("### ventoy_block_io_ramdisk_read sector:%u count:%u", (UINT32)Lba, (UINT32)BufferSize / 2048);

    (VOID)This;
    (VOID)MediaId;

    CopyMem(Buffer, (char *)g_chain + (Lba * 2048), BufferSize);

	return EFI_SUCCESS;
}

EFI_LBA EFIAPI ventoy_fixup_iso9660_sector(IN EFI_LBA Lba, UINT32 secNum)
{
    UINT32 i = 0;

    if (g_fixup_iso9660_secover_cur_secs > 0)
    {
        Lba += VENTOY_ISO9660_SECTOR_OVERFLOW;
        g_fixup_iso9660_secover_cur_secs += secNum;
        if (g_fixup_iso9660_secover_cur_secs >= g_fixup_iso9660_secover_tot_secs)
        {
            g_fixup_iso9660_secover_start = FALSE;
            goto end;
        }
    }
    else
    {
        ventoy_iso9660_override *dirent;
        ventoy_override_chunk *pOverride;

        for (i = 0, pOverride = g_override_chunk; i < g_override_chunk_num; i++, pOverride++)
        {
            dirent = (ventoy_iso9660_override *)pOverride->override_data;
            if (Lba == dirent->first_sector)
            {
                g_fixup_iso9660_secover_start = FALSE;
                goto end;
            }
        }

        if (g_fixup_iso9660_secover_start)
        {
            for (i = 0, pOverride = g_override_chunk; i < g_override_chunk_num; i++, pOverride++)
            {
                dirent = (ventoy_iso9660_override *)pOverride->override_data;
                if (Lba + VENTOY_ISO9660_SECTOR_OVERFLOW == dirent->first_sector)
                {
                    g_fixup_iso9660_secover_tot_secs = (dirent->size + 2047) / 2048;
                    g_fixup_iso9660_secover_cur_secs = secNum;
                    if (g_fixup_iso9660_secover_cur_secs >= g_fixup_iso9660_secover_tot_secs)
                    {
                        g_fixup_iso9660_secover_start = FALSE;
                    }
                    Lba += VENTOY_ISO9660_SECTOR_OVERFLOW;
                    goto end;
                }
            }
        }
    }

end:
    return Lba;
}

EFI_STATUS EFIAPI ventoy_block_io_read 
(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                          MediaId,
    IN EFI_LBA                         Lba,
    IN UINTN                           BufferSize,
    OUT VOID                          *Buffer
) 
{
    UINT32 i = 0;
    UINT32 j = 0;
    UINT32 lbacount = 0;
    UINT32 secNum = 0;
    UINT64 offset = 0;
    EFI_LBA curlba = 0;
    EFI_LBA lastlba = 0;
    UINT8 *lastbuffer;
    ventoy_sector_flag *cur_flag;
    ventoy_virt_chunk *node;
    
    //debug("### ventoy_block_io_read sector:%u count:%u", (UINT32)Lba, (UINT32)BufferSize / 2048);

    secNum = BufferSize / 2048;

    /* Workaround for SSTR PE loader error */
    if (g_fixup_iso9660_secover_start)
    {
        Lba = ventoy_fixup_iso9660_sector(Lba, secNum);
    }

    offset = Lba * 2048;

    if (offset + BufferSize <= g_chain->real_img_size_in_bytes)
    {
        return ventoy_read_iso_sector(Lba, secNum, Buffer);
    }

    if (secNum > g_sector_flag_num)
    {
        cur_flag = AllocatePool(secNum * sizeof(ventoy_sector_flag));
        if (NULL == cur_flag)
        {
            return EFI_OUT_OF_RESOURCES;
        }

        FreePool(g_sector_flag);
        g_sector_flag = cur_flag;
        g_sector_flag_num = secNum;
    }

    for (curlba = Lba, cur_flag = g_sector_flag, j = 0; j < secNum; j++, curlba++, cur_flag++)
    {
        cur_flag->flag = 0;
        for (node = g_virt_chunk, i = 0; i < g_virt_chunk_num; i++, node++)
        {
            if (curlba >= node->mem_sector_start && curlba < node->mem_sector_end)
            {
                CopyMem((UINT8 *)Buffer + j * 2048, 
                       (char *)g_virt_chunk + node->mem_sector_offset + (curlba - node->mem_sector_start) * 2048,
                       2048);
                cur_flag->flag = 1;
                break;
            }
            else if (curlba >= node->remap_sector_start && curlba < node->remap_sector_end)
            {
                cur_flag->remap_lba = node->org_sector_start + curlba - node->remap_sector_start;
                cur_flag->flag = 2;
                break;
            }
        }
    }

    for (curlba = Lba, cur_flag = g_sector_flag, j = 0; j < secNum; j++, curlba++, cur_flag++)
    {
        if (cur_flag->flag == 2)
        {
            if (lastlba == 0)
            {
                lastbuffer = (UINT8 *)Buffer + j * 2048;
                lastlba = cur_flag->remap_lba;
                lbacount = 1;
            }
            else if (lastlba + lbacount == cur_flag->remap_lba)
            {
                lbacount++;
            }
            else
            {
                ventoy_read_iso_sector(lastlba, lbacount, lastbuffer);
                lastbuffer = (UINT8 *)Buffer + j * 2048;
                lastlba = cur_flag->remap_lba;
                lbacount = 1;
            }
        }
    }

    if (lbacount > 0)
    {
        ventoy_read_iso_sector(lastlba, lbacount, lastbuffer);
    }

	return EFI_SUCCESS;
}

EFI_STATUS EFIAPI ventoy_block_io_write 
(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                          MediaId,
    IN EFI_LBA                         Lba,
    IN UINTN                           BufferSize,
    IN VOID                           *Buffer
) 
{
    (VOID)This;
    (VOID)MediaId;
    (VOID)Lba;
    (VOID)BufferSize;
    (VOID)Buffer;
	return EFI_WRITE_PROTECTED;
}

EFI_STATUS EFIAPI ventoy_block_io_flush(IN EFI_BLOCK_IO_PROTOCOL *This)
{
	(VOID)This;
	return EFI_SUCCESS;
}


EFI_STATUS EFIAPI ventoy_fill_device_path(VOID)
{
    UINTN NameLen = 0;
    UINT8 TmpBuf[128] = {0};
    VENDOR_DEVICE_PATH *venPath = NULL;

    venPath = (VENDOR_DEVICE_PATH *)TmpBuf;
    NameLen = StrSize(VTOY_BLOCK_DEVICE_PATH_NAME);
    venPath->Header.Type = HARDWARE_DEVICE_PATH;
    venPath->Header.SubType = HW_VENDOR_DP;
    venPath->Header.Length[0] = sizeof(VENDOR_DEVICE_PATH) + NameLen;
    venPath->Header.Length[1] = 0;
    CopyMem(&venPath->Guid, &gVtoyBlockDevicePathGuid, sizeof(EFI_GUID));
    CopyMem(venPath + 1, VTOY_BLOCK_DEVICE_PATH_NAME, NameLen);
    
    gBlockData.Path = AppendDevicePathNode(NULL, (EFI_DEVICE_PATH_PROTOCOL *)TmpBuf);
    gBlockData.DevicePathCompareLen = sizeof(VENDOR_DEVICE_PATH) + NameLen;

    debug("gBlockData.Path=<%s>\n", ConvertDevicePathToText(gBlockData.Path, FALSE, FALSE));

    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI ventoy_connect_driver(IN EFI_HANDLE ControllerHandle, IN CONST CHAR16 *DrvName)
{
    UINTN i = 0;
    UINTN Count = 0;
    CHAR16 *DriverName = NULL;
    EFI_HANDLE *Handles = NULL;
    EFI_HANDLE DrvHandles[2] = { NULL };
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_COMPONENT_NAME_PROTOCOL *NameProtocol = NULL;
    EFI_COMPONENT_NAME2_PROTOCOL *Name2Protocol = NULL;

    debug("ventoy_connect_driver <%s>...", DrvName);

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiComponentName2ProtocolGuid, 
                                     NULL, &Count, &Handles);
    if (EFI_ERROR(Status))
    {
        return Status;
    }

    for (i = 0; i < Count; i++)
    {
        Status = gBS->HandleProtocol(Handles[i], &gEfiComponentName2ProtocolGuid, (VOID **)&Name2Protocol);
        if (EFI_ERROR(Status))
        {
            continue;
        }

        Status = Name2Protocol->GetDriverName(Name2Protocol, "en", &DriverName);
        if (EFI_ERROR(Status) || NULL == DriverName)
        {
            continue;
        }

        if (StrStr(DriverName, DrvName))
        {
            debug("Find driver name2:<%s>: <%s>", DriverName, DrvName);
            DrvHandles[0] = Handles[i];
            break;
        }
    }

    if (i < Count)
    {
        Status = gBS->ConnectController(ControllerHandle, DrvHandles, NULL, TRUE);
        debug("Connect partition driver:<%r>", Status);
        goto end;
    }

    debug("%s NOT found, now try COMPONENT_NAME", DrvName);

    Count = 0;
    FreePool(Handles);
    Handles = NULL;

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiComponentNameProtocolGuid, 
                                     NULL, &Count, &Handles);
    if (EFI_ERROR(Status))
    {
        return Status;
    }

    for (i = 0; i < Count; i++)
    {
        Status = gBS->HandleProtocol(Handles[i], &gEfiComponentNameProtocolGuid, (VOID **)&NameProtocol);
        if (EFI_ERROR(Status))
        {
            continue;
        }

        Status = NameProtocol->GetDriverName(NameProtocol, "en", &DriverName);
        if (EFI_ERROR(Status))
        {
            continue;
        }

        if (StrStr(DriverName, DrvName))
        {
            debug("Find driver name:<%s>: <%s>", DriverName, DrvName);
            DrvHandles[0] = Handles[i];
            break;
        }
    }

    if (i < Count)
    {
        Status = gBS->ConnectController(ControllerHandle, DrvHandles, NULL, TRUE);
        debug("Connect partition driver:<%r>", Status);
        goto end;
    }

    Status = EFI_NOT_FOUND;
    
end:
    FreePool(Handles);
    
    return Status;
}

EFI_STATUS EFIAPI ventoy_install_blockio(IN EFI_HANDLE ImageHandle, IN UINT64 ImgSize)
{   
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_BLOCK_IO_PROTOCOL *pBlockIo = &(gBlockData.BlockIo);
    
    ventoy_fill_device_path();
    
    gBlockData.Media.BlockSize = 2048;
    gBlockData.Media.LastBlock = ImgSize / 2048 - 1;
    gBlockData.Media.ReadOnly = TRUE;
    gBlockData.Media.MediaPresent = 1;
    gBlockData.Media.LogicalBlocksPerPhysicalBlock = 1;

	pBlockIo->Revision = EFI_BLOCK_IO_PROTOCOL_REVISION3;
	pBlockIo->Media = &(gBlockData.Media);
	pBlockIo->Reset = ventoy_block_io_reset;
    pBlockIo->ReadBlocks = gMemdiskMode ? ventoy_block_io_ramdisk_read : ventoy_block_io_read;
	pBlockIo->WriteBlocks = ventoy_block_io_write;
	pBlockIo->FlushBlocks = ventoy_block_io_flush;

    Status = gBS->InstallMultipleProtocolInterfaces(&gBlockData.Handle,
            &gEfiBlockIoProtocolGuid, &gBlockData.BlockIo,
            &gEfiDevicePathProtocolGuid, gBlockData.Path,
            NULL);
    debug("Install protocol %r %p", Status, gBlockData.Handle);
    if (EFI_ERROR(Status))
    {
        return Status;
    }
    
    Status = ventoy_connect_driver(gBlockData.Handle, L"Disk I/O Driver");
    debug("Connect disk IO driver %r", Status);
    ventoy_debug_pause();
    
    Status = ventoy_connect_driver(gBlockData.Handle, L"Partition Driver");
    debug("Connect partition driver %r", Status);
    if (EFI_ERROR(Status))
    {
        Status = gBS->ConnectController(gBlockData.Handle, NULL, NULL, TRUE);
        debug("Connect all controller %r", Status);
    }

    ventoy_debug_pause();

    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI ventoy_wrapper_file_open
(
    EFI_FILE_HANDLE This, 
    EFI_FILE_HANDLE *New,
    CHAR16 *Name, 
    UINT64 Mode, 
    UINT64 Attributes
)
{
    UINT32 i = 0;
    UINT32 j = 0;
    UINT64 Sectors = 0;
    EFI_STATUS Status = EFI_SUCCESS;
    CHAR8 TmpName[256];
    ventoy_virt_chunk *virt = NULL;

    Status = g_original_fopen(This, New, Name, Mode, Attributes);
    if (EFI_ERROR(Status))
    {
        return Status;
    }

    if (g_file_replace_list && g_file_replace_list->magic == GRUB_FILE_REPLACE_MAGIC &&
        g_file_replace_list->new_file_virtual_id < g_virt_chunk_num)
    {
        AsciiSPrint(TmpName, sizeof(TmpName), "%s", Name);
        for (j = 0; j < 4; j++)
        {
            if (0 == AsciiStrCmp(g_file_replace_list[i].old_file_name[j], TmpName))
            {
                g_original_fclose(*New);
                *New = &g_efi_file_replace.WrapperHandle;
                ventoy_wrapper_file_procotol(*New);

                virt = g_virt_chunk + g_file_replace_list->new_file_virtual_id;

                Sectors = (virt->mem_sector_end - virt->mem_sector_start) + (virt->remap_sector_end - virt->remap_sector_start);
                
                g_efi_file_replace.BlockIoSectorStart = virt->mem_sector_start;
                g_efi_file_replace.FileSizeBytes = Sectors * 2048;

                if (gDebugPrint)
                {
                    debug("## ventoy_wrapper_file_open <%s> BlockStart:%lu Sectors:%lu Bytes:%lu", Name,
                        g_efi_file_replace.BlockIoSectorStart, Sectors, Sectors * 2048);
                    sleep(3);
                }
                
                return Status;
            }
        }
    }

    return Status;
}

EFI_STATUS EFIAPI ventoy_wrapper_open_volume
(
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     *This,
    OUT EFI_FILE_PROTOCOL                 **Root
)
{
    EFI_STATUS Status = EFI_SUCCESS;
    
    Status = g_original_open_volume(This, Root);
    if (!EFI_ERROR(Status))
    {
        g_original_fopen = (*Root)->Open;
        g_original_fclose = (*Root)->Close;
        (*Root)->Open = ventoy_wrapper_file_open;
    }

    return Status;
}

EFI_STATUS EFIAPI ventoy_wrapper_push_openvolume(IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume)
{
    g_original_open_volume = OpenVolume;
    return EFI_SUCCESS;
}

