# Makefile for the kernel cdrom device drivers.
#
# 30 Jan 1998, Michael Elizabeth Chastain, <mailto:mec@shout.net>
# Rewritten to use lists instead of if-statements.



# All of the (potential) objects that export symbols.
# This list comes from 'grep -l EXPORT_SYMBOL *.[hc]'.

export-objs	:= cdrom.o



# Object file lists.

obj-y		:=
obj-m		:=
obj-n		:=
obj-		:=



# Each configuration option enables a list of files.

obj-$(CONFIG_BLK_DEV_IDECD)	+=              cdrom.o
obj-$(CONFIG_BLK_DEV_SR)	+=              cdrom.o
obj-$(CONFIG_PARIDE_PCD)	+=		cdrom.o

obj-$(CONFIG_AZTCD)		+= aztcd.o
obj-$(CONFIG_CDU31A)		+= cdu31a.o     cdrom.o
obj-$(CONFIG_CM206)		+= cm206.o      cdrom.o
obj-$(CONFIG_GSCD)		+= gscd.o
obj-$(CONFIG_ISP16_CDI)		+= isp16.o
obj-$(CONFIG_MCD)		+= mcd.o        cdrom.o
obj-$(CONFIG_MCDX)		+= mcdx.o       cdrom.o
obj-$(CONFIG_OPTCD)		+= optcd.o
obj-$(CONFIG_SBPCD)		+= sbpcd.o      cdrom.o
obj-$(CONFIG_SBPCD2)		+= sbpcd2.o     cdrom.o
obj-$(CONFIG_SBPCD3)		+= sbpcd3.o     cdrom.o
obj-$(CONFIG_SBPCD4)		+= sbpcd4.o     cdrom.o
obj-$(CONFIG_SJCD)		+= sjcd.o
obj-$(CONFIG_CDU535)		+= sonycd535.o



# Files that are both resident and modular: remove from modular.

obj-m		:= $(filter-out $(obj-y), $(obj-m))



# Translate to Rules.make lists.

L_TARGET	:= cdrom.a
MOD_LIST_NAME	:= CDROM_MODULES

L_OBJS		:= $(sort $(filter-out $(export-objs), $(obj-y)))
LX_OBJS		:= $(sort $(filter     $(export-objs), $(obj-y)))
M_OBJS		:= $(sort $(filter-out $(export-objs), $(obj-m)))
MX_OBJS		:= $(sort $(filter     $(export-objs), $(obj-m)))



# Hand off to Rules.make.

include $(TOPDIR)/Rules.make
