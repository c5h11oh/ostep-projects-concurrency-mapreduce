make:
	gcc -o mapreduce mapreduce.c main.c mapreduce.h -Wall -Werror -pthread -g
clean:
	rm -f mapreduce