PYTHON ?= python3
EXAMPLE ?= examples/01_mlp_layer.mt
ARGS ?=

.PHONY: test run

test:
	PYTHONPATH=src $(PYTHON) -m unittest discover -s tests -v

run:
	PYTHONPATH=src $(PYTHON) -m minitensor $(ARGS) $(EXAMPLE)
