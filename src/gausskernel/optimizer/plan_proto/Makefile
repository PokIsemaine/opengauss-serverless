#---------------------------------------------------------------------------------------
#     
# IDENTIFICATION
#        src/gausskernel/optimizer/plan/Makefile
#
# ---------------------------------------------------------------------------------------

subdir = src/gausskernel/optimizer/plan_proto
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
    ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
      -include $(DEPEND)
    endif
  endif
endif

OBJS = enumtag.pb.o execNodes.pb.o query_plan.pb.o query_stmt.pb.o SerializePlan.o

include $(top_srcdir)/src/gausskernel/common.mk
