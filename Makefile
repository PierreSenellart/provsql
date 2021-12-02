INTERNAL = Makefile.internal
ARGS = with_llvm=no

default:
	$(MAKE) -f $(INTERNAL) $(ARGS)

%:
	$(MAKE) -f $(INTERNAL) $@ $(ARGS)

test:
	bash -c "set -o pipefail && make installcheck | tee test.log" || pager `grep regression.diffs test.log | perl -pe 's/.*?"//;s/".*//'`

.PHONY: default test

tdkc: src/TreeDecomposition.cpp src/TreeDecomposition.h src/BooleanCircuit.cpp src/BooleanCircuit.h src/Circuit.hpp src/dDNNF.h src/dDNNF.cpp src/dDNNFTreeDecompositionBuilder.h src/dDNNFTreeDecompositionBuilder.cpp src/Circuit.h src/Graph.h src/PermutationStrategy.h src/TreeDecompositionKnowledgeCompiler.cpp
	$(CXX) -std=c++17 -DTDKC -W -Wall -o tdkc src/TreeDecomposition.cpp src/BooleanCircuit.cpp src/dDNNF.cpp src/dDNNFTreeDecompositionBuilder.cpp src/TreeDecompositionKnowledgeCompiler.cpp

docker-build:
	make clean
	docker build -f docker/Dockerfile .
