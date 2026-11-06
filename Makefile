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
		--about "Unlocks Pad Perform / Scale Mode on devices where the firmware has it but the UI hides it"

deploy: build
	@test -n "$(MPC_HOST)" || { echo "set MPC_HOST (e.g. MPC_HOST=root@192.168.x.x)"; exit 1; }
	scp build/mpc_enable_scalemode.so build/padctl mpc_enable_scalemode.conf \
		$(MPC_HOST):/data/mpc-addins/enable-scalemode/
