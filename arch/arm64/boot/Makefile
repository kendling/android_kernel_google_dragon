#
# arch/arm64/boot/Makefile
#
# This file is included by the global makefile so that you can add your own
# architecture-specific flags and dependencies.
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 2012, ARM Ltd.
# Author: Will Deacon <will.deacon@arm.com>
#
# Based on the ia64 boot/Makefile.
#

targets := Image Image.gz Image.fit

$(obj)/Image: vmlinux FORCE
	$(call if_changed,objcopy)

quiet_cmd_fitimage = BUILD   $@
cmd_fitimage = $(CONFIG_SHELL) $(srctree)/chromeos/scripts/generate-its-script.sh \
	       -a $(ARCH) -c lzma $< $(obj)/dts/tegra/*.dtb | \
	       $(objtree)/scripts/dtc/dtc -I dts -O dtb -p 1024 > $@

$(obj)/Image.fit: $(obj)/Image $(DTB_OBJS) FORCE
	$(call if_changed,fitimage)

$(obj)/Image.gz: $(obj)/Image FORCE
	$(call if_changed,gzip)

$(obj)/Image.gz-dtb: $(obj)/Image.gz $(DTB_OBJS) FORCE
	$(call if_changed,cat)

install: $(obj)/Image
	$(CONFIG_SHELL) $(srctree)/$(src)/install.sh $(KERNELRELEASE) \
	$(obj)/Image System.map "$(INSTALL_PATH)"

zinstall: $(obj)/Image.gz
	$(CONFIG_SHELL) $(srctree)/$(src)/install.sh $(KERNELRELEASE) \
	$(obj)/Image.gz System.map "$(INSTALL_PATH)"
