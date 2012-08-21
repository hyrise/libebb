PREFIX = $(HOME)/local/libebb

# libev
EVINC  = $(HOME)/local/libev/include
EVLIB  = $(HOME)/local/libev/lib
EVLIBS = -L${EVLIB} -lev

# includes and libs
INCS = -I${EVINC}
LIBS = ${EVLIBS} #-lefence

# flags
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS   = -O2 -g -Wall ${INCS} ${CPPFLAGS} -fPIC
LDFLAGS  = -s ${LIBS}
LDOPT    = -shared
SUFFIX   = so
SONAME   = -Wl,-soname,$(OUTPUT_LIB)

# Solaris
#CFLAGS  = -fast ${INCS} -DVERSION=\"$(VERSION)\" -fPIC
#LDFLAGS = ${LIBS}
#SONAME  = 

# Darwin
# LDOPT  = -dynamiclib 
# SUFFIX = dylib
# SONAME = -current_version $(VERSION) -compatibility_version $(VERSION)

# compiler and linker
CC = cc
RANLIB = ranlib
