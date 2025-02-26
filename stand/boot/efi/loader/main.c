/*-
 * Copyright (c) 2008-2010 Rui Paulo
 * Copyright (c) 2006 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: head/sys/boot/efi/loader/main.c 295408 2016-02-08 19:34:17Z imp $
 */

#include <sys/param.h>
#include <sys/reboot.h>
#include <sys/boot.h>
#include <stand.h>
#include <string.h>
#include <setjmp.h>

#include <efi.h>
#include <efilib.h>

#include <uuid.h>

#include <bootstrap.h>
#include <smbios.h>

#include "loader_efi.h"

extern char bootprog_name[];
extern char bootprog_rev[];
extern char bootprog_date[];
extern char bootprog_maker[];

struct arch_switch archsw;	/* MI/MD interface boundary */

EFI_GUID acpi = ACPI_TABLE_GUID;
EFI_GUID acpi20 = EFI_ACPI_TABLE_GUID;
EFI_GUID devid = DEVICE_PATH_PROTOCOL;
EFI_GUID imgid = LOADED_IMAGE_PROTOCOL;
EFI_GUID mps = EFI_MPS_TABLE_GUID;
EFI_GUID netid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
EFI_GUID smbios = SMBIOS_TABLE_GUID;
EFI_GUID dxe = DXE_SERVICES_TABLE_GUID;
EFI_GUID hoblist = HOB_LIST_GUID;
EFI_GUID memtype = MEMORY_TYPE_INFORMATION_TABLE_GUID;
EFI_GUID debugimg = EFI_DEBUG_IMAGE_INFO_TABLE_GUID;
EFI_GUID fdtdtb = FDT_TABLE_GUID;
EFI_GUID inputid = SIMPLE_INPUT_PROTOCOL;

/*
 * Need this because EFI uses UTF-16 unicode string constants, but we
 * use UTF-8. We can't use printf due to the possibility of \0 and we
 * don't support support wide characters either.
 */
static void
print_str16(const CHAR16 *str)
{
	int i;

	for (i = 0; str[i]; i++)
		printf("%c", (char)str[i]);
}

/*
 * cpy8to16 copies a traditional C string into a CHAR16 string and
 * 0 terminates it. len is the size of *dst in bytes.
 */
static void
cpy8to16(const char *src, CHAR16 *dst, size_t len)
{
	len <<= 1;		/* Assume CHAR16 is 2 bytes */
	while (len > 0 && *src) {
		*dst++ = *src++;
		len--;
	}
	*dst++ = (CHAR16)0;
}

static void
cp16to8(const CHAR16 *src, char *dst, size_t len)
{
	size_t i;

	for (i = 0; i < len && src[i]; i++)
		dst[i] = (char)src[i];
}

static int
has_keyboard(void)
{
	EFI_STATUS status;
	EFI_DEVICE_PATH *path;
	EFI_HANDLE *hin, *hin_end, *walker;
	UINTN sz;
	int retval = 0;
	
	/*
	 * Find all the handles that support the SIMPLE_TEXT_INPUT_PROTOCOL and
	 * do the typical dance to get the right sized buffer.
	 */
	sz = 0;
	hin = NULL;
	status = BS->LocateHandle(ByProtocol, &inputid, 0, &sz, 0);
	if (status == EFI_BUFFER_TOO_SMALL) {
		hin = (EFI_HANDLE *)malloc(sz);
		status = BS->LocateHandle(ByProtocol, &inputid, 0, &sz,
		    hin);
		if (EFI_ERROR(status))
			free(hin);
	}
	if (EFI_ERROR(status))
		return retval;

	/*
	 * Look at each of the handles. If it supports the device path protocol,
	 * use it to get the device path for this handle. Then see if that
	 * device path matches either the USB device path for keyboards or the
	 * legacy device path for keyboards.
	 */
	hin_end = &hin[sz / sizeof(*hin)];
	for (walker = hin; walker < hin_end; walker++) {
		status = OpenProtocolByHandle(*walker, &devid, (VOID **)&path);
		if (EFI_ERROR(status))
			continue;

		while (!IsDevicePathEnd(path)) {
			/*
			 * Check for the ACPI keyboard node. All PNP3xx nodes
			 * are keyboards of different flavors. Note: It is
			 * unclear of there's always a keyboard node when
			 * there's a keyboard controller, or if there's only one
			 * when a keyboard is detected at boot.
			 */
			if (DevicePathType(path) == ACPI_DEVICE_PATH &&
			    (DevicePathSubType(path) == ACPI_DP ||
				DevicePathSubType(path) == ACPI_EXTENDED_DP)) {
				ACPI_HID_DEVICE_PATH  *acpi;

				acpi = (ACPI_HID_DEVICE_PATH *)(void *)path;
				if ((EISA_ID_TO_NUM(acpi->HID) & 0xff00) == 0x300 &&
				    (acpi->HID & 0xffff) == PNP_EISA_ID_CONST) {
					retval = 1;
					goto out;
				}
			/*
			 * Check for USB keyboard node, if present. Unlike a
			 * PS/2 keyboard, these definitely only appear when
			 * connected to the system.
			 */
			} else if (DevicePathType(path) == MESSAGING_DEVICE_PATH &&
			    DevicePathSubType(path) == MSG_USB_CLASS_DP) {
				USB_CLASS_DEVICE_PATH *usb;
			       
				usb = (USB_CLASS_DEVICE_PATH *)(void *)path;
				if (usb->DeviceClass == 3 && /* HID */
				    usb->DeviceSubClass == 1 && /* Boot devices */
				    usb->DeviceProtocol == 1) { /* Boot keyboards */
					retval = 1;
					goto out;
				}
			}
			path = NextDevicePathNode(path);
		}
	}
out:
	free(hin);
	return retval;
}

EFI_STATUS
main(int argc, CHAR16 *argv[])
{
	char var[128];
	EFI_LOADED_IMAGE *img;
	EFI_GUID *guid;
	int i, j, vargood, unit, howto;
	struct devsw *dev;
	uint64_t pool_guid;
	UINTN k;
	int has_kbd;

	archsw.arch_autoload = efi_autoload;
	archsw.arch_getdev = efi_getdev;
	archsw.arch_copyin = efi_copyin;
	archsw.arch_copyout = efi_copyout;
	archsw.arch_readin = efi_readin;

	has_kbd = has_keyboard();

	/*
	 * XXX Chicken-and-egg problem; we want to have console output
	 * early, but some console attributes may depend on reading from
	 * eg. the boot device, which we can't do yet.  We can use
	 * printf() etc. once this is done.
	 */
	cons_probe();

	/*
	 * Parse the args to set the console settings, etc
	 * boot1.efi passes these in, if it can read /boot.config or /boot/config
	 * or iPXE may be setup to pass these in.
	 *
	 * Loop through the args, and for each one that contains an '=' that is
	 * not the first character, add it to the environment.  This allows
	 * loader and kernel env vars to be passed on the command line.  Convert
	 * args from UCS-2 to ASCII (16 to 8 bit) as they are copied.
	 */
	howto = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			for (j = 1; argv[i][j] != 0; j++) {
				int ch;

				ch = argv[i][j];
				switch (ch) {
				case 'a':
					howto |= RB_ASKNAME;
					break;
				case 'd':
					howto |= RB_KDB;
					break;
				case 'h':
					howto |= RB_SERIAL;
					break;
				case 'm':
					howto |= RB_MUTE;
					break;
				case 'p':
					howto |= RB_PAUSE;
					break;
				case 'P':
					if (!has_kbd) {
						howto &= ~(RB_MUTE|RB_VIDEO);
						howto |= RB_SERIAL;
					}
					break;
				case 'r':
					howto |= RB_DFLTROOT;
					break;
				case 's':
					howto |= RB_SINGLE;
					break;
				case 'S':
					if (argv[i][j + 1] == 0) {
						if (i + 1 == argc) {
							setenv("comconsole_speed", "115200", 1);
						} else {
							cp16to8(&argv[i + 1][0], var,
							    sizeof(var));
							setenv("comconsole_speedspeed", var, 1);
						}
						i++;
						break;
					} else {
						cp16to8(&argv[i][j + 1], var,
						    sizeof(var));
						setenv("comconsole_speed", var, 1);
						break;
					}
				case 'v':
					howto |= RB_VERBOSE;
					break;
				}
			}
		} else {
			vargood = 0;
			for (j = 0; argv[i][j] != 0; j++) {
				if (j == sizeof(var)) {
					vargood = 0;
					break;
				}
				if (j > 0 && argv[i][j] == '=')
					vargood = 1;
				var[j] = (char)argv[i][j];
			}
			if (vargood) {
				var[j] = 0;
				putenv(var);
			}
		}
	}
	for (i = 0; howto_names[i].ev != NULL; i++)
		if (howto & howto_names[i].mask)
			setenv(howto_names[i].ev, "YES", 1);
	if (howto & RB_SERIAL) {
		setenv("console", "comconsole" , 1);
	}

	if (efi_copy_init()) {
		printf("failed to allocate staging area\n");
		return (EFI_BUFFER_TOO_SMALL);
	}

	/*
	 * March through the device switch probing for things.
	 */
	for (i = 0; devsw[i] != NULL; i++)
		if (devsw[i]->dv_init != NULL)
			(devsw[i]->dv_init)();

	/* Get our loaded image protocol interface structure. */
	OpenProtocolByHandle(IH, &imgid, (VOID**)&img);

	printf("Command line arguments:");
	for (i = 0; i < argc; i++) {
		printf(" ");
		print_str16(argv[i]);
	}
	printf("\n");

	printf("Image base: 0x%lx\n", (u_long)img->ImageBase);
	printf("EFI version: %d.%02d\n", ST->Hdr.Revision >> 16,
	    ST->Hdr.Revision & 0xffff);
	printf("EFI Firmware: ");
	/* printf doesn't understand EFI Unicode */
	ST->ConOut->OutputString(ST->ConOut, ST->FirmwareVendor);
	printf(" (rev %d.%02d)\n", ST->FirmwareRevision >> 16,
	    ST->FirmwareRevision & 0xffff);

	printf("\n");
	printf("%s, Revision %s\n", bootprog_name, bootprog_rev);
	printf("(%s, %s)\n", bootprog_maker, bootprog_date);

	/*
	 * Disable the watchdog timer. By default the boot manager sets
	 * the timer to 5 minutes before invoking a boot option. If we
	 * want to return to the boot manager, we have to disable the
	 * watchdog timer and since we're an interactive program, we don't
	 * want to wait until the user types "quit". The timer may have
	 * fired by then. We don't care if this fails. It does not prevent
	 * normal functioning in any way...
	 */
	BS->SetWatchdogTimer(0, 0, 0, NULL);

	if (efi_handle_lookup(img->DeviceHandle, &dev, &unit, &pool_guid) != 0)
		return (EFI_NOT_FOUND);

	switch (dev->dv_type) {
	default: {
		struct efi_devdesc currdev;

		currdev.d_dev = dev;
		currdev.d_kind.efidisk.unit = unit;
		currdev.d_kind.efidisk.data = NULL;
		currdev.d_type = currdev.d_dev->dv_type;
		env_setenv("currdev", EV_VOLATILE, efi_fmtdev(&currdev),
			   efi_setcurrdev, env_nounset);
		env_setenv("loaddev", EV_VOLATILE, efi_fmtdev(&currdev), env_noset,
			   env_nounset);
		break;
	}
	}

	/* enable EHCI */
	setenv("ehci_load", "YES", 1);

	/* enable XHCI */
	setenv("xhci_load", "YES", 1);

	/* Check if ACPI is available */
	if (efi_get_table(&acpi20) != NULL ||
	    efi_get_table(&acpi) != NULL) {
		setenv("acpi_load", "YES", 1);
	}

	setenv("LINES", "24", 1);	/* optional */

	for (k = 0; k < ST->NumberOfTableEntries; k++) {
		guid = &ST->ConfigurationTable[k].VendorGuid;
		if (!memcmp(guid, &smbios, sizeof(EFI_GUID))) {
			smbios_detect(ST->ConfigurationTable[k].VendorTable);
			break;
		}
	}

	interact();			/* doesn't return */

	return (EFI_SUCCESS);		/* keep compiler happy */
}

/* XXX move to lib stand ? */
static int
wcscmp(CHAR16 *a, CHAR16 *b)
{

	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return *a - *b;
}


COMMAND_SET(reboot, "reboot", "reboot the system", command_reboot);

static int
command_reboot(int argc, char *argv[])
{
	int i;

	for (i = 0; devsw[i] != NULL; ++i)
		if (devsw[i]->dv_cleanup != NULL)
			(devsw[i]->dv_cleanup)();

	RS->ResetSystem(EfiResetCold, EFI_SUCCESS, 23,
	    (CHAR16 *)"Reboot from the loader");

	/* NOTREACHED */
	return (CMD_ERROR);
}

COMMAND_SET(quit, "quit", "exit the loader", command_quit);

static int
command_quit(int argc, char *argv[])
{
	exit(0);
	return (CMD_OK);
}

COMMAND_SET(memmap, "memmap", "print memory map", command_memmap);

static int
command_memmap(int argc, char *argv[])
{
	UINTN sz;
	EFI_MEMORY_DESCRIPTOR *map, *p;
	UINTN key, dsz;
	UINT32 dver;
	EFI_STATUS status;
	int i, ndesc;
	char line[80];
	static char *types[] = {
	    "Reserved",
	    "LoaderCode",
	    "LoaderData",
	    "BootServicesCode",
	    "BootServicesData",
	    "RuntimeServicesCode",
	    "RuntimeServicesData",
	    "ConventionalMemory",
	    "UnusableMemory",
	    "ACPIReclaimMemory",
	    "ACPIMemoryNVS",
	    "MemoryMappedIO",
	    "MemoryMappedIOPortSpace",
	    "PalCode"
	};

	sz = 0;
	status = BS->GetMemoryMap(&sz, 0, &key, &dsz, &dver);
	if (status != EFI_BUFFER_TOO_SMALL) {
		printf("Can't determine memory map size\n");
		return (CMD_ERROR);
	}
	map = malloc(sz);
	status = BS->GetMemoryMap(&sz, map, &key, &dsz, &dver);
	if (EFI_ERROR(status)) {
		printf("Can't read memory map\n");
		return (CMD_ERROR);
	}

	ndesc = sz / dsz;
	snprintf(line, sizeof(line), "%23s %12s %12s %8s %4s\n",
	    "Type", "Physical", "Virtual", "#Pages", "Attr");
	pager_open();
	if (pager_output(line)) {
		pager_close();
		return (CMD_OK);
	}

	for (i = 0, p = map; i < ndesc;
	     i++, p = NextMemoryDescriptor(p, dsz)) {
		printf("%23s %012jx %012jx %08jx ", types[p->Type],
		    (uintmax_t)p->PhysicalStart, (uintmax_t)p->VirtualStart,
		    (uintmax_t)p->NumberOfPages);
		if (p->Attribute & EFI_MEMORY_UC)
			printf("UC ");
		if (p->Attribute & EFI_MEMORY_WC)
			printf("WC ");
		if (p->Attribute & EFI_MEMORY_WT)
			printf("WT ");
		if (p->Attribute & EFI_MEMORY_WB)
			printf("WB ");
		if (p->Attribute & EFI_MEMORY_UCE)
			printf("UCE ");
		if (p->Attribute & EFI_MEMORY_WP)
			printf("WP ");
		if (p->Attribute & EFI_MEMORY_RP)
			printf("RP ");
		if (p->Attribute & EFI_MEMORY_XP)
			printf("XP ");
		if (pager_output("\n"))
			break;
	}

	pager_close();
	return (CMD_OK);
}

COMMAND_SET(configuration, "configuration", "print configuration tables",
    command_configuration);

static const char *
guid_to_string(EFI_GUID *guid)
{
	static char buf[40];

	sprintf(buf, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    guid->Data1, guid->Data2, guid->Data3, guid->Data4[0],
	    guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4],
	    guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	return (buf);
}

static int
command_configuration(int argc, char *argv[])
{
	char line[80];
	UINTN i;

	snprintf(line, sizeof(line), "NumberOfTableEntries=%lu\n",
		(unsigned long)ST->NumberOfTableEntries);
	pager_open();
	if (pager_output(line)) {
		pager_close();
		return (CMD_OK);
	}

	for (i = 0; i < ST->NumberOfTableEntries; i++) {
		EFI_GUID *guid;

		printf("  ");
		guid = &ST->ConfigurationTable[i].VendorGuid;
		if (!memcmp(guid, &mps, sizeof(EFI_GUID)))
			printf("MPS Table");
		else if (!memcmp(guid, &acpi, sizeof(EFI_GUID)))
			printf("ACPI Table");
		else if (!memcmp(guid, &acpi20, sizeof(EFI_GUID)))
			printf("ACPI 2.0 Table");
		else if (!memcmp(guid, &smbios, sizeof(EFI_GUID)))
			printf("SMBIOS Table");
		else if (!memcmp(guid, &dxe, sizeof(EFI_GUID)))
			printf("DXE Table");
		else if (!memcmp(guid, &hoblist, sizeof(EFI_GUID)))
			printf("HOB List Table");
		else if (!memcmp(guid, &memtype, sizeof(EFI_GUID)))
			printf("Memory Type Information Table");
		else if (!memcmp(guid, &debugimg, sizeof(EFI_GUID)))
			printf("Debug Image Info Table");
		else if (!memcmp(guid, &fdtdtb, sizeof(EFI_GUID)))
			printf("FDT Table");
		else
			printf("Unknown Table (%s)", guid_to_string(guid));
		snprintf(line, sizeof(line), " at %p\n",
		    ST->ConfigurationTable[i].VendorTable);
		if (pager_output(line))
			break;
	}

	pager_close();
	return (CMD_OK);
}


COMMAND_SET(mode, "mode", "change or display EFI text modes", command_mode);

static int
command_mode(int argc, char *argv[])
{
	UINTN cols, rows;
	unsigned int mode;
	int i;
	char *cp;
	char rowenv[8];
	EFI_STATUS status;
	SIMPLE_TEXT_OUTPUT_INTERFACE *conout;
	extern void HO(void);

	conout = ST->ConOut;

	if (argc > 1) {
		mode = strtol(argv[1], &cp, 0);
		if (cp[0] != '\0') {
			printf("Invalid mode\n");
			return (CMD_ERROR);
		}
		status = conout->QueryMode(conout, mode, &cols, &rows);
		if (EFI_ERROR(status)) {
			printf("invalid mode %d\n", mode);
			return (CMD_ERROR);
		}
		status = conout->SetMode(conout, mode);
		if (EFI_ERROR(status)) {
			printf("couldn't set mode %d\n", mode);
			return (CMD_ERROR);
		}
		sprintf(rowenv, "%u", (unsigned)rows);
		setenv("LINES", rowenv, 1);
		HO();		/* set cursor */
		return (CMD_OK);
	}

	printf("Current mode: %d\n", conout->Mode->Mode);
	for (i = 0; i <= conout->Mode->MaxMode; i++) {
		status = conout->QueryMode(conout, i, &cols, &rows);
		if (EFI_ERROR(status))
			continue;
		printf("Mode %d: %u columns, %u rows\n", i, (unsigned)cols,
		    (unsigned)rows);
	}

	if (i != 0)
		printf("Select a mode with the command \"mode <number>\"\n");

	return (CMD_OK);
}

COMMAND_SET(efishow, "efi-show", "print some or all EFI variables", command_efi_show);

static int
efi_print_var(CHAR16 *varnamearg, EFI_GUID *matchguid, int lflag)
{
	UINTN		datasz, i;
	EFI_STATUS	status;
	UINT32		attr;
	CHAR16		*data;
	char		*str;
	uint32_t	uuid_status;
	int		is_ascii;

	datasz = 0;
	status = RS->GetVariable(varnamearg, matchguid, &attr,
	    &datasz, NULL);
	if (status != EFI_BUFFER_TOO_SMALL) {
		printf("Can't get the variable: error %#llx\n", status);
		return (CMD_ERROR);
	}
	data = malloc(datasz);
	status = RS->GetVariable(varnamearg, matchguid, &attr,
	    &datasz, data);
	if (status != EFI_SUCCESS) {
		printf("Can't get the variable: error %#llx\n", status);
		return (CMD_ERROR);
	}
	uuid_to_string((uuid_t *)matchguid, &str, &uuid_status);
	if (lflag) {
		printf("%s 0x%x %S", str, attr, varnamearg);
	} else {
		printf("%s 0x%x %S=", str, attr, varnamearg);
		is_ascii = 1;
		free(str);
		str = (char *)data;
		for (i = 0; i < datasz - 1; i++) {
			/* Quick hack to see if this ascii-ish string printable range plus tab, cr and lf */
			if ((str[i] < 32 || str[i] > 126) && str[i] != 9 && str[i] != 10 && str[i] != 13) {
				is_ascii = 0;
				break;
			}
		}
		if (str[datasz - 1] != '\0')
			is_ascii = 0;
		if (is_ascii)
			printf("%s", str);
		else {
			for (i = 0; i < datasz / 2; i++) {
				if (isalnum(data[i]) || isspace(data[i]))
					printf("%c", data[i]);
				else
					printf("\\x%02x", data[i]);
			}
		}
	}
	free(data);
	pager_output("\n");
	return (CMD_OK);
}

static int
command_efi_show(int argc, char *argv[])
{
	/*
	 * efi-show [-a]
	 *	print all the env
	 * efi-show -u UUID
	 *	print all the env vars tagged with UUID
	 * efi-show -v var
	 *	search all the env vars and print the ones matching var
	 * eif-show -u UUID -v var
	 * eif-show UUID var
	 *	print all the env vars that match UUID and var
	 */
	/* NB: We assume EFI_GUID is the same as uuid_t */
	int		aflag = 0, gflag = 0, lflag = 0, vflag = 0;
	int		ch, rv;
	unsigned	i;
	EFI_STATUS	status;
	EFI_GUID	varguid = { 0,0,0,{0,0,0,0,0,0,0,0} };
	EFI_GUID	matchguid = { 0,0,0,{0,0,0,0,0,0,0,0} };
	uint32_t	uuid_status;
	CHAR16		*varname;
	CHAR16		*newnm;
	CHAR16		varnamearg[128];
	UINTN		varalloc;
	UINTN		varsz;

	while ((ch = getopt(argc, argv, "ag:lv:")) != -1) {
		switch (ch) {
		case 'a':
			aflag = 1;
			break;
		case 'g':
			gflag = 1;
			uuid_from_string(optarg, (uuid_t *)&matchguid,
			    &uuid_status);
			if (uuid_status != uuid_s_ok) {
				printf("uid %s could not be parsed\n", optarg);
				return (CMD_ERROR);
			}
			break;
		case 'l':
			lflag = 1;
			break;
		case 'v':
			vflag = 1;
			if (strlen(optarg) >= nitems(varnamearg)) {
				printf("Variable %s is longer than %zu characters\n",
				    optarg, nitems(varnamearg));
				return (CMD_ERROR);
			}
			for (i = 0; i < strlen(optarg); i++)
				varnamearg[i] = optarg[i];
			varnamearg[i] = 0;
			break;
		default:
			printf("Invalid argument %c\n", ch);
			return (CMD_ERROR);
		}
	}

	if (aflag && (gflag || vflag)) {
		printf("-a isn't compatible with -v or -u\n");
		return (CMD_ERROR);
	}

	if (aflag && optind < argc) {
		printf("-a doesn't take any args");
		return (CMD_ERROR);
	}

	if (optind == argc)
		aflag = 1;

	argc -= optind;
	argv += optind;

	pager_open();
	if (vflag && gflag) {
		rv = efi_print_var(varnamearg, &matchguid, lflag);
		pager_close();
		return (rv);
	}

	if (argc == 2) {
		optarg = argv[0];
		if (strlen(optarg) >= nitems(varnamearg)) {
			printf("Variable %s is longer than %zu characters\n",
			    optarg, nitems(varnamearg));
			pager_close();
			return (CMD_ERROR);
		}
		for (i = 0; i < strlen(optarg); i++)
			varnamearg[i] = optarg[i];
		varnamearg[i] = 0;
		optarg = argv[1];
		uuid_from_string(optarg, (uuid_t *)&matchguid,
		    &uuid_status);
		if (uuid_status != uuid_s_ok) {
			printf("uid %s could not be parsed\n", optarg);
			pager_close();
			return (CMD_ERROR);
		}
		rv = efi_print_var(varnamearg, &matchguid, lflag);
		pager_close();
		return (rv);
	}

	if (argc > 0) {
		printf("Too many args %d\n", argc);
		pager_close();
		return (CMD_ERROR);
	}

	/*
	 * Initiate the search -- note the standard takes pain
	 * to specify the initial call must be a poiner to a NULL
	 * character.
	 */
	varalloc = 1024;
	varname = malloc(varalloc);
	if (varname == NULL) {
		printf("Can't allocate memory to get variables\n");
		pager_close();
		return (CMD_ERROR);
	}
	varname[0] = 0;
	while (1) {
		varsz = varalloc;
		status = RS->GetNextVariableName(&varsz, varname, &varguid);
		if (status == EFI_BUFFER_TOO_SMALL) {
			varalloc = varsz;
			newnm = malloc(varalloc);
			if (newnm == NULL) {
				printf("Can't allocate memory to get variables\n");
				free(varname);
				pager_close();
				return (CMD_ERROR);
			}
			memcpy(newnm, varname, varsz);
			free(varname);
			varname = newnm;
			continue; /* Try again with bigger buffer */
		}
		if (status != EFI_SUCCESS)
			break;
		if (aflag) {
			if (efi_print_var(varname, &varguid, lflag) != CMD_OK)
				break;
			continue;
		}
		if (vflag) {
			if (wcscmp(varnamearg, varname) == 0) {
				if (efi_print_var(varname, &varguid, lflag) != CMD_OK)
					break;
				continue;
			}
		}
		if (gflag) {
			if (memcmp(&varguid, &matchguid, sizeof(varguid)) == 0) {
				if (efi_print_var(varname, &varguid, lflag) != CMD_OK)
					break;
				continue;
			}
		}
	}
	free(varname);
	pager_close();

	return (CMD_OK);
}

COMMAND_SET(efiset, "efi-set", "set EFI variables", command_efi_set);

static int
command_efi_set(int argc, char *argv[])
{
	char *uuid, *var, *val;
	CHAR16 wvar[128];
	EFI_GUID guid;
	uint32_t status;
	EFI_STATUS err;

	if (argc != 4) {
		printf("efi-set uuid var new-value\n");
		return (CMD_ERROR);
	}
	uuid = argv[1];
	var = argv[2];
	val = argv[3];
	uuid_from_string(uuid, (uuid_t *)&guid, &status);
	if (status != uuid_s_ok) {
		printf("Invalid uuid %s %d\n", uuid, status);
		return (CMD_ERROR);
	}
	cpy8to16(var, wvar, sizeof(wvar));
	err = RS->SetVariable(wvar, &guid,
	    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS,
	    strlen(val) + 1, val);
	if (EFI_ERROR(err)) {
		printf("Failed to set variable: error %llu\n", err);
		return (CMD_ERROR);
	}
	return (CMD_OK);
}

COMMAND_SET(efiunset, "efi-unset", "delete / unset EFI variables", command_efi_unset);

static int
command_efi_unset(int argc, char *argv[])
{
	char *uuid, *var;
	CHAR16 wvar[128];
	EFI_GUID guid;
	uint32_t status;
	EFI_STATUS err;

	if (argc != 3) {
		printf("efi-unset uuid var\n");
		return (CMD_ERROR);
	}
	uuid = argv[1];
	var = argv[2];
	uuid_from_string(uuid, (uuid_t *)&guid, &status);
	if (status != uuid_s_ok) {
		printf("Invalid uuid %s\n", uuid);
		return (CMD_ERROR);
	}
	cpy8to16(var, wvar, sizeof(wvar));
	err = RS->SetVariable(wvar, &guid, 0, 0, NULL);
	if (EFI_ERROR(err)) {
		printf("Failed to unset variable: error %llu\n", err);
		return (CMD_ERROR);
	}
	return (CMD_OK);
}
