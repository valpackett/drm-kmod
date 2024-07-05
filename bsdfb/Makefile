# SRCDIR=	${.CURDIR:H}/drivers/dma-buf

# .PATH:	${SRCDIR}

.include "../kconfig.mk"
.include "../linuxkpi_version.mk"

KMOD=	bsdfb
SRCS=	bsdfb.c

SRCS+=	opt_splash.h \
	opt_syscons.h \
	opt_teken.h

SRCS+=	device_if.h \
	bus_if.h \
	pci_if.h \
	vnode_if.h

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/dummy/include # fallback to dummy
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include

CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
# CFLAGS+= -I${SRCDIR:H:H} -I${SRCDIR} -I${SRCDIR}/display
CFLAGS+= -I${.CURDIR:H}/drivers/gpu

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= ${KCONFIG:C/(.*)/-DCONFIG_\1/}

CWARNFLAGS.bsdfb.c=		-Wno-gnu-pointer-arith

.include <bsd.kmod.mk>
