CC=g++
CFLAGS=-Wall -Werror -O3 -march=native -fopenmp -lgomp -I$(BOOST_DIR)/include
EXECUTABLE=./count_bits

$(EXECUTABLE): count_bits.cpp
	$(CC) $(CFLAGS) $< -o $@

test: $(EXECUTABLE)
	$(EXECUTABLE) 20

clean:
	rm -f $(EXECUTABLE) core*

.PHONY: clean test
