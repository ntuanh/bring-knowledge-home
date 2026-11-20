# Top-level Makefile.
#   make build   build the mini C compiler (build/minic)
#   make test    run every test suite (minic, minitensor, visualizer API)
#   make run     start the visualizer on a free port
#   make clean   remove build output

PYTHON ?= python3
MINIC_DIR := part1-c-compiler/minic

.PHONY: build test test-minic test-minitensor test-api run clean check-tools

build: check-tools
	$(MAKE) --no-print-directory -C $(MINIC_DIR)

check-tools:
	@missing=""; \
	for t in gcc as ld make $(PYTHON) readelf objdump nm size strings file ss; do \
		command -v $$t >/dev/null 2>&1 || missing="$$missing $$t"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing tools:$$missing"; \
		echo "Install with: sudo apt install build-essential binutils python3 file iproute2"; \
		exit 1; \
	fi

test: build test-minic test-minitensor test-api
	@echo "ALL TEST SUITES PASSED"

test-minic: build
	bash $(MINIC_DIR)/tests/run_tests.sh

test-minitensor:
	cd part2-ai-compiler && $(PYTHON) -m unittest discover -s tests -v

test-api: build
	$(PYTHON) -m unittest discover -s visualizer/tests -v

run: build
	$(PYTHON) visualizer/server.py

clean:
	rm -rf build
	find . -name __pycache__ -type d -prune -exec rm -rf {} +
