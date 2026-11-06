.PHONY: build clean release deploy

VERSION := $(shell cat VERSION)
REPO    := jacob-sabella/mpc-addin-sample-scalemode

build:
	bash build.sh

clean:
	rm -rf build/

release: build
	python3 ../mpc-vst-plugins/tools/release_addin.py \
		--dir build --version $(VERSION) \
		--repo $(REPO) --license MIT \
		--depends "sample-draw-handler>=0.5.0" \
		--about "Hold 16 Levels on the MPC Sample to play its 16 slots in a scale, set on a SCALE page"

deploy: build
	@test -n "$(MPC_HOST)" || { echo "set MPC_HOST (e.g. MPC_HOST=root@192.168.x.x)"; exit 1; }
	scp build/mpc_sample_scalemode.so build/padctl mpc_sample_scalemode.conf \
		$(MPC_HOST):/data/mpc-addins/sample-scalemode/
