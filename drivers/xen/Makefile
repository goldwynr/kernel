xen-biomerge-$(CONFIG_PARAVIRT_XEN) := biomerge.o
xen-hotplug-$(CONFIG_PARAVIRT_XEN) := cpu_hotplug.o
xen-balloon_$(CONFIG_PARAVIRT_XEN) := xen-balloon.o
xen-evtchn-name-$(CONFIG_PARAVIRT_XEN) := xen-evtchn
xen-privcmd_$(CONFIG_PARAVIRT_XEN) := xen-privcmd.o

ifeq ($(filter y, $(CONFIG_ARM) $(CONFIG_ARM64)),)
obj-$(CONFIG_HOTPLUG_CPU)	+= $(xen-hotplug-y)
endif
obj-$(CONFIG_X86)		+= fallback.o
obj-$(CONFIG_PARAVIRT_XEN)	+= grant-table.o features.o balloon.o manage.o
obj-$(CONFIG_PARAVIRT_XEN)	+= events/

xen-balloon_$(CONFIG_XEN)	:= balloon/
xen-privcmd_$(CONFIG_XEN)	:= privcmd/
obj-$(CONFIG_XEN)		+= core/
obj-$(CONFIG_XEN)		+= console/
obj-y				+= xenbus/
obj-$(CONFIG_XEN)		+= char/

xen-backend-$(CONFIG_XEN_BACKEND)	:= util.o
xen-evtchn-name-$(CONFIG_XEN)		:= evtchn

nostackp := $(call cc-option, -fno-stack-protector)
ifeq ($(CONFIG_PARAVIRT_XEN),y)
CFLAGS_features.o			:= $(nostackp)
endif
CFLAGS_efi.o				+= -fshort-wchar

priv-$(CONFIG_USB_SUPPORT)		:= dbgp.o
priv-$(CONFIG_PCI)			+= pci.o

obj-$(CONFIG_XEN)			+= features.o $(xen-backend-y) $(xen-backend-m)
obj-$(CONFIG_XEN_PRIVILEGED_GUEST)	+= $(priv-y)
dom0-$(CONFIG_PCI) += pci.o
dom0-$(CONFIG_USB_SUPPORT) += dbgp.o
dom0-$(CONFIG_ACPI) += acpi.o $(xen-pad-y)
xen-pad-$(CONFIG_X86) += xen-acpi-pad.o
dom0-$(CONFIG_X86) += pcpu.o
obj-$(CONFIG_XEN_DOM0)			+= $(dom0-y)
obj-$(CONFIG_BLOCK)			+= $(xen-biomerge-y)
obj-$(CONFIG_XEN_BALLOON)		+= $(xen-balloon_y)
obj-$(CONFIG_XEN_SELFBALLOONING)	+= xen-selfballoon.o
obj-$(CONFIG_XEN_DEV_EVTCHN)		+= $(xen-evtchn-name-y).o
obj-$(CONFIG_XEN_GNTDEV)		+= xen-gntdev.o
obj-$(CONFIG_XENFS)			+= xenfs/
obj-$(CONFIG_XEN_GRANT_DEV_ALLOC)	+= xen-gntalloc.o
obj-$(CONFIG_XEN_SYS_HYPERVISOR)	+= sys-hypervisor.o
obj-$(CONFIG_XEN_PVHVM)			+= platform-pci.o
obj-$(CONFIG_XEN_TMEM)			+= tmem.o
obj-$(CONFIG_SWIOTLB_XEN)		+= swiotlb-xen.o
obj-$(CONFIG_XEN_MCE_LOG)		+= mcelog.o
obj-$(CONFIG_XEN_PCIDEV_BACKEND)	+= xen-pciback/
obj-$(CONFIG_XEN_PRIVCMD)		+= $(xen-privcmd_y)
obj-$(CONFIG_XEN_STUB)			+= xen-stub.o
obj-$(CONFIG_XEN_ACPI_HOTPLUG_MEMORY)	+= xen-acpi-memhotplug.o
obj-$(CONFIG_XEN_ACPI_HOTPLUG_CPU)	+= xen-acpi-cpuhotplug.o
obj-$(CONFIG_XEN_ACPI_PROCESSOR)	+= xen-acpi-processor.o
obj-$(CONFIG_XEN_EFI)			+= efi.o
obj-$(CONFIG_PARAVIRT_XEN_SCSI_BACKEND)	+= xen-scsiback.o
xen-evtchn-y				:= evtchn.o
xen-gntdev-y				:= gntdev.o
xen-gntalloc-y				:= gntalloc.o
xen-privcmd-y				:= privcmd.o

obj-$(CONFIG_XEN_BLKDEV_BACKEND)	+= blkback/
obj-$(CONFIG_XEN_BLKDEV_TAP)		+= blktap/
obj-$(filter m,$(CONFIG_XEN_BLKDEV_TAP2)) += blktap2/ blktap2-new/
obj-$(CONFIG_XEN_BLKDEV_TAP2_LEGACY)	+= blktap2/
obj-$(CONFIG_XEN_BLKDEV_TAP2_NEW)	+= blktap2-new/
obj-$(CONFIG_XEN_NETDEV_BACKEND)	+= netback/
obj-$(CONFIG_XEN_TPMDEV_BACKEND)	+= tpmback/
obj-$(CONFIG_XEN_BLKDEV_FRONTEND)	+= blkfront/
obj-$(CONFIG_XEN_NETDEV_FRONTEND)	+= netfront/
obj-$(CONFIG_XEN_PCIDEV_FRONTEND)	+= pcifront/
obj-$(CONFIG_XEN_FRAMEBUFFER)		+= fbfront/
obj-$(CONFIG_XEN_KEYBOARD)		+= fbfront/
obj-$(CONFIG_XEN_SCSI_BACKEND)		+= scsiback/
obj-$(CONFIG_XEN_SCSI_FRONTEND)		+= scsifront/
obj-$(CONFIG_XEN_USB_BACKEND)		+= usbback/
obj-$(CONFIG_XEN_USB_FRONTEND)		+= usbfront/
obj-$(CONFIG_XEN_GRANT_DEV)	+= gntdev/
obj-$(CONFIG_XEN_NETDEV_ACCEL_SFC_UTIL)		+= sfc_netutil/
obj-$(CONFIG_XEN_NETDEV_ACCEL_SFC_FRONTEND)	+= sfc_netfront/
obj-$(CONFIG_XEN_NETDEV_ACCEL_SFC_BACKEND)	+= sfc_netback/
