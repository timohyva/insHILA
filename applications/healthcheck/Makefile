#%     make [..] NDIM=<d>    - Dimensions of system, default 3

# Give the location of the top level distribution directory wrt. this.
# Can be absolute or relative
HILA_DIR := /projappl/project_2017849/Quann/libs/insHILA

# Set default goal and arch
.DEFAULT_GOAL := hila_healthcheck

APP_HEADERS := 

ARCH := vanilla

NDIM := 3

APP_OPTS := -DNDIM=$(NDIM) -DSPECIAL_BOUNDARY_CONDITIONS

# Read in the main makefile contents, incl. platforms
include $(HILA_DIR)/libraries/main.mk


# With multiple targets we want to use "make target", not "make build/target".
# This is needed to carry the dependencies to build-subdir

hila_healthcheck: build/hila_healthcheck ; @:

build/hila_healthcheck: Makefile build/Targets/hila_healthcheck.o $(HILA_OBJECTS) $(HEADERS) 
	$(LD) -o $@ build/Targets/hila_healthcheck.o $(HILA_OBJECTS) $(LDFLAGS) $(LDLIBS)

