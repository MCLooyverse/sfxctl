mclLoc = ~/Programs

sfxctl : main.o
	g++ -o sfxctl main.o

main.o : main.cpp
	g++ -c main.cpp --std=c++20 -I $(mclLoc)