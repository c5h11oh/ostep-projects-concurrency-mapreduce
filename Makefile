make:
	gcc -o mapreduce-executable mapreduce.c main.c mapreduce.h -Wall -Werror -pthread -g
test:
	./runtest.sh
clean:
	rm -f mapreduce-executable
	rm -rf testresult/