#pragma once
#include "defs.hpp"

namespace utils
{
	PVOID get_system_information(SYSTEM_INFORMATION_CLASS information_class)
	{
		ULONG size = 32;
		char buffer[32];
		ZwQuerySystemInformation(information_class, buffer, size, &size);
		PVOID info = ExAllocatePoolZero(NonPagedPool, size, 7265746172);
		if (!info) return 0;
		if (ZwQuerySystemInformation(information_class, info, size, &size) != STATUS_SUCCESS)
		{
			ExFreePool(info);
			return 0;
		}
		return info;
	}
	uintptr_t get_kernel_module(const char* name)
	{
		const PRTL_PROCESS_MODULES info = (PRTL_PROCESS_MODULES)get_system_information(SystemModuleInformation);
		if (!info) return NULL;
		for (size_t i = 0; i < info->NumberOfModules; ++i)
		{
			const auto& module = info->Modules[i];
			if (strcmp(to_lower((char*)module.FullPathName + module.OffsetToFileName), name) == 0)
			{
				const PVOID address = module.ImageBase;
				ExFreePool(info);
				return (uintptr_t)address;
			}
		}
		ExFreePool(info);
		return NULL;
	}
	uintptr_t pattern_scan(uintptr_t base, size_t range, const char* pattern, const char* mask)
	{
		const auto check_mask = [](const char* base, const char* pattern, const char* mask) -> bool
		{
			for (; *mask; ++base, ++pattern, ++mask)
			{
				if (*mask == 'x' && *base != *pattern) return false;
			}
			return true;
		};
		range = range - strlen(mask);
		for (size_t i = 0; i < range; ++i)
		{
			if (check_mask((const char*)base + i, pattern, mask)) return base + i;
		}
		return NULL;
	}
	uintptr_t pattern_scan(uintptr_t base, const char* pattern, const char* mask)
	{
		const PIMAGE_NT_HEADERS headers = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
		const PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(headers);
		for (size_t i = 0; i < headers->FileHeader.NumberOfSections; i++)
		{
			const PIMAGE_SECTION_HEADER section = &sections[i];
			if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				const uintptr_t match = pattern_scan(base + section->VirtualAddress, section->Misc.VirtualSize, pattern, mask);
				if (match) return match;
			}
		}
		return 0;
	}
	ULONG get_winver()
	{
		RTL_OSVERSIONINFOW ver = { 0 };
		RtlGetVersion(&ver);
		switch (ver.dwBuildNumber)
		{
		case win10_1803:
		case win10_1809:
			return 0x0278;
		case win10_1903:
		case win10_1909:
			return 0x0280;
		case win10_2004:
		case win10_20h2:
		case win10_21h1:
		case win10_21h2:
		case win10_22h2:
			return 0x0388;
		case win11_21h2:
		case win11_22h2:
			return 0x0390;
		default:
			return 0x0390;
		}
	}
	uintptr_t get_process_cr3(PEPROCESS pprocess)
	{
		if (!pprocess) return 0;
		PUCHAR process = (PUCHAR)pprocess;
		uintptr_t process_dirbase = *(uintptr_t*)(process + 0x28);
		if (process_dirbase == 0)
		{
			ULONG user_diroffset = get_winver();
			uintptr_t process_userdirbase = *(uintptr_t*)(process + user_diroffset);
			return process_userdirbase;
		}
		return process_dirbase;
	}
	NTSTATUS write_physical_memory(PVOID address, PVOID buffer, SIZE_T size, PSIZE_T bytes)
	{
		if (!address) return STATUS_UNSUCCESSFUL;
		PHYSICAL_ADDRESS to_write = { 0 };
		to_write.QuadPart = (LONGLONG)address;
		PVOID pmapped_mem = MmMapIoSpaceEx(to_write, size, PAGE_READWRITE);
		if (!pmapped_mem) return STATUS_UNSUCCESSFUL;
		memcpy(pmapped_mem, buffer, size);
		*bytes = size;
		MmUnmapIoSpace(pmapped_mem, size);
		return STATUS_SUCCESS;
	}
	NTSTATUS read_physical_memory(PVOID address, PVOID buffer, SIZE_T size, PSIZE_T bytes)
	{
		if (!address) return STATUS_UNSUCCESSFUL;
		MM_COPY_ADDRESS to_read = { 0 };
		to_read.PhysicalAddress.QuadPart = (LONGLONG)address;
		return MmCopyMemory(buffer, to_read, size, MM_COPY_MEMORY_PHYSICAL, bytes);
	}
	uintptr_t translate_linear(uintptr_t directory_table_base, uintptr_t virtual_address)
	{
		directory_table_base &= ~0xf;
		uintptr_t pageoffset = virtual_address & ~(~0ul << page_offset_size);
		uintptr_t pte = ((virtual_address >> 12) & (0x1ffll));
		uintptr_t pt = ((virtual_address >> 21) & (0x1ffll));
		uintptr_t pd = ((virtual_address >> 30) & (0x1ffll));
		uintptr_t pdp = ((virtual_address >> 39) & (0x1ffll));
		SIZE_T readsize = 0;
		uintptr_t pdpe = 0;
		read_physical_memory((PVOID)(directory_table_base + 8 * pdp), &pdpe, sizeof(pdpe), &readsize);
		if (~pdpe & 1) return 0;
		uintptr_t pde = 0;
		read_physical_memory((PVOID)((pdpe & pmask) + 8 * pd), &pde, sizeof(pde), &readsize);
		if (~pde & 1) return 0;
		if (pde & 0x80) return (pde & (~0ull << 42 >> 12)) + (virtual_address & ~(~0ull << 30));
		uintptr_t ptraddr = 0;
		read_physical_memory((PVOID)((pde & pmask) + 8 * pt), &ptraddr, sizeof(ptraddr), &readsize);
		if (~ptraddr & 1) return 0;
		if (ptraddr & 0x80) return (ptraddr & pmask) + (virtual_address & ~(~0ull << 21));
		virtual_address = 0;
		read_physical_memory((PVOID)((ptraddr & pmask) + 8 * pte), &virtual_address, sizeof(virtual_address), &readsize);
		virtual_address &= pmask;
		if (!virtual_address) return 0;
		return virtual_address + pageoffset;
	}
	NTSTATUS get_base_address(PDRIVER_REQUEST in)
	{
		if (!in->pid) return STATUS_UNSUCCESSFUL;
		PEPROCESS process = NULL;
		PsLookupProcessByProcessId(in->pid, &process);
		if (!process) return STATUS_UNSUCCESSFUL;
		PVOID image_base = PsGetProcessSectionBaseAddress(process);
		if (!image_base) return STATUS_UNSUCCESSFUL;
		RtlCopyMemory(in->base, &image_base, sizeof(image_base));
		ObDereferenceObject(process);
		return STATUS_SUCCESS;
	}
	NTSTATUS write_process_memory(PDRIVER_REQUEST in)
	{
		if (!in->pid) return STATUS_UNSUCCESSFUL;
		PEPROCESS process = NULL;
		PsLookupProcessByProcessId(in->pid, &process);
		if (!process) return STATUS_UNSUCCESSFUL;
		uintptr_t process_base = get_process_cr3(process);
		ObDereferenceObject(process);
		uintptr_t physical_address = translate_linear(process_base, (uintptr_t)in->address);
		if (!physical_address) return STATUS_UNSUCCESSFUL;
		uintptr_t final_size = min(PAGE_SIZE - (physical_address & 0xFFF), in->size);
		SIZE_T bytes_trough = NULL;
		write_physical_memory((PVOID)physical_address, in->buffer, final_size, &bytes_trough);
		return STATUS_SUCCESS;
	}
	NTSTATUS read_process_memory(PDRIVER_REQUEST in)
	{
		if (!in->pid) return STATUS_UNSUCCESSFUL;
		PEPROCESS process = NULL;
		PsLookupProcessByProcessId(in->pid, &process);
		if (!process) return STATUS_UNSUCCESSFUL;
		uintptr_t process_base = get_process_cr3(process);
		if (!process_base) return STATUS_UNSUCCESSFUL;
		ObDereferenceObject(process);
		uintptr_t physical_address = translate_linear(process_base, (uintptr_t)in->address);
		if (!physical_address) return STATUS_UNSUCCESSFUL;
		uintptr_t final_size = min(PAGE_SIZE - (physical_address & 0xFFF), in->size);
		SIZE_T bytes_trough = NULL;
		read_physical_memory((PVOID)physical_address, in->buffer, final_size, &bytes_trough);
		return STATUS_SUCCESS;
	}
}