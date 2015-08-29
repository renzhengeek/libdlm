all install clean: %:
	for d in libdlm dlm_controld dlm_tool fence; do $(MAKE) -C $$d $@; done
