.PHONY: clean

c2cbw: c2cbw.cpp
	g++ $< -O3 -o $@

clean:
	rm c2cbw || true