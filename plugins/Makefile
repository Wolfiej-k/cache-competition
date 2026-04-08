ifeq ($(V),true)
    FILTER =
else
	FILTER = 2>&1 | grep -vE "\[(INFO|DEBUG|WARN)\]"
endif

build: *.cpp
	mkdir -p build && cd build/ && cmake -G Ninja .. && ninja

run-physics:
	@../_build/bin/cachesim ../data/cloudPhysicsIO.vscsi vscsi pluginCache \
		$(size) -e "plugin_path=build/libplugin_$(alg)_hooks.so" \
		$(FILTER)

run-twitter:
	@../_build/bin/cachesim ../data/twitter_cluster52.csv csv pluginCache \
		$(size) -e "plugin_path=build/libplugin_$(alg)_hooks.so" \
		-t "time-col=1, obj-id-col=2, obj-size-col=4, obj-id-is-num=true" \
		$(FILTER)

run-all: run-physics run-twitter
