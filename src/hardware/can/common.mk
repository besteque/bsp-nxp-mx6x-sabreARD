#
# Copyright 2004, QNX Software Systems Ltd. All Rights Reserved
#
# This source code has been published by QNX Software Systems Ltd. (QSSL).
# However, any use, reproduction, modification, distribution or transfer of
# this software, or any software which includes or is based upon any of this
# code, is only permitted under the terms of the QNX Community License version
# 1.0 (see licensing.qnx.com for details) or as otherwise expressly authorized
# by a written license agreement from QSSL. For more information, please email
# licensing@qnx.com.
#
#
# General purpose makefile for building a Neutrino CAN device driver
#
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

ifndef CLASS
CLASS=can
endif

ifndef NAME
NAME=dev-$(CLASS)-$(SECTION)
endif

USEFILE=$(SECTION_ROOT)/$(SECTION).use
EXTRA_SILENT_VARIANTS+=$(SECTION)
EXTRA_SRCVPATH = $(EXTRA_SRCVPATH_$(SECTION))

LIBS_am335x += can drvr
LIBS_am3517 += io-can drvr
LIBS_mx35 += io-can drvr
LIBS_mx6x += can drvr
LIBS_rcar += can drvr
LIBS_topcliff += io-can drvr
LIBS_peak_minipcie += io-can drvr
LIBS += $(LIBS_$(SECTION))

define PINFO
PINFO DESCRIPTION=
endef

include $(MKFILES_ROOT)/qmacros.mk

-include $(SECTION_ROOT)/extra_libs.mk

include $(SECTION_ROOT)/pinfo.mk

-include $(PROJECT_ROOT)/roots.mk


#####AUTO-GENERATED by packaging script... do not checkin#####
   INSTALL_ROOT_nto = $(PROJECT_ROOT)/../../../install
   USE_INSTALL_ROOT=1
##############################################################

include $(MKFILES_ROOT)/qtargets.mk

