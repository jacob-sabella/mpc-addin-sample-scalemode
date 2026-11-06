.PHONY: build clean release deploy

VERSION := $(shell cat VERSION)
REPO    := jacob-sabella/mpc-addin-enable-scalemode

build:
	bash build.sh

clean:
	rm -rf build/

release: build
	python3 ../mpc-vst-plugins/tools/release_addin.py \
		--dir build --version $(VERSION) \
		--repo $(REPO) --license MIT \
		--about "Adds Scale to the MPC Sample's 16 Levels Type cycle (Shift+16 Levels)"

deploy: build
	@test -n "$(MPC_HOST)" || { echo "set MPC_HOST (e.g. MPC_HOST=root@192.168.x.x)"; exit 1; }
	scp build/mpc_enable_scalemode.so build/padctl mpc_enable_scalemode.conf \
		$(MPC_HOST):/data/mpc-addins/enable-scalemode/
