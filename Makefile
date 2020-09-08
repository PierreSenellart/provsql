INTERNAL = Makefile.internal
ARGS = with_llvm=no

default:
	$(MAKE) -f $(INTERNAL) $(ARGS)

%:
	$(MAKE) -f $(INTERNAL) $@ $(ARGS)

.PHONY: default
