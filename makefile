
ALL_OBJ := server.o  

app:$(ALL_OBJ)


%.o:%.c
	gcc-7 $< -o $@ -lpthread -g -Wall -Werror 
