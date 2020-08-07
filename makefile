
ALL_OBJ := server.o  

app:$(ALL_OBJ)


%.o:%.c
	gcc $< -o $@ -lpthread -g -Wall -Werror 
