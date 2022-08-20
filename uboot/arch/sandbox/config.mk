# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2011 The Chromium OS Authors.

PLATFORM_CPPFLAGS += -D__SANDBOX__ -U_FORTIFY_SOURCE
PLATFORM_CPPFLAGS += -DCONFIG_ARCH_MAP_SYSMEM
PLATFORM_CPPFLAGS += -fPIC
PLATFORM_LIBS += -lrt
SDL_CONFIG ?= sdl2-config

# Define this to avoid linking with SDL, which requires SDL libraries
# This can solve 'sdl-config: Command not found' errors
ifneq ($(NO_SDL),)
PLATFORM_CPPFLAGS += -DSANDBOX_NO_SDL
else
PLATFORM_LIBS += $(shell $(SDL_CONFIG) --libs)
PLATFORM_CPPFLAGS += $(shell $(SDL_CONFIG) --cflags)
endif

cmd_u-boot__ = $(CC) -o $@ -Wl,-T u-boot.lds $(u-boot-init) \
	-Wl,--start-group $(u-boot-main) -Wl,--end-group \
	$(PLATFORM_LIBS) -Wl,-Map -Wl,u-boot.map

cmd_u-boot-spl = (cd $(obj) && $(CC) -o $(SPL_BIN) -Wl,-T u-boot-spl.lds \
	$(patsubst $(obj)/%,%,$(u-boot-spl-init)) \
	-Wl,--start-group $(patsubst $(obj)/%,%,$(u-boot-spl-main)) \
	$(patsubst $(obj)/%,%,$(u-boot-spl-platdata)) -Wl,--end-group \
	$(PLATFORM_LIBS) -Wl,-Map -Wl,u-boot-spl.map -Wl,--gc-sections)

CONFIG_ARCH_DEVICE_TREE := sandbox

ifeq ($(HOST_ARCH),$(HOST_ARCH_X86_64))
EFI_LDS := ${SRCDIR}/../../../arch/x86/lib/elf_x86_64_efi.lds
EFI_TARGET := --target=efi-app-x86_64
else ifeq ($(HOST_ARCH),$(HOST_ARCH_X86))
EFI_LDS := ${SRCDIR}/../../../arch/x86/lib/elf_ia32_efi.lds
EFI_TARGET := --target=efi-app-ia32
else ifeq ($(HOST_ARCH),$(HOST_ARCH_AARCH64))
EFI_LDS := ${SRCDIR}/../../../arch/arm/lib/elf_aarch64_efi.lds
OBJCOPYFLAGS += -j .text -j .secure_text -j .secure_data -j .rodata -j .data \
		-j .u_boot_list -j .rela.dyn -j .got -j .got.plt \
		-j .binman_sym_table -j .text_rest \
		-j .efi_runtime -j .efi_runtime_rel
else ifeq ($(HOST_ARCH),$(HOST_ARCH_ARM))
EFI_LDS := ${SRCDIR}/../../../arch/arm/lib/elf_arm_efi.lds
OBJCOPYFLAGS += -j .text -j .secure_text -j .secure_data -j .rodata -j .hash \
		-j .data -j .got -j .got.plt -j .u_boot_list -j .rel.dyn \
		-j .binman_sym_table -j .text_rest \
		-j .efi_runtime -j .efi_runtime_rel
else ifeq ($(HOST_ARCH),$(HOST_ARCH_RISCV32))
EFI_LDS := ${SRCDIR}/../../../arch/riscv/lib/elf_riscv32_efi.lds
else ifeq ($(HOST_ARCH),$(HOST_ARCH_RISCV64))
EFI_LDS := ${SRCDIR}/../../../arch/riscv/lib/elf_riscv64_efi.lds
endif
EFI_CRT0 := crt0_sandbox_efi.o
EFI_RELOC := reloc_sandbox_efi.o
AFLAGS_crt0_sandbox_efi.o += -DHOST_ARCH="$(HOST_ARCH)"
CFLAGS_reloc_sandbox_efi.o += -DHOST_ARCH="$(HOST_ARCH)"
