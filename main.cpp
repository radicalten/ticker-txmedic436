#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <curl/curl.h>

#include "stock.h"

#define RED "\033[31m"
#define GREEN "\033[32m"
#define RESET "\033[0m"

size_t Callback(void* buffer, size_t size, size_t num, void* out);
std::string URLGenerator(std::string symbol);

const std::string UP_ARROW = "\u2191";
const std::string DOWN_ARROW = "\u2193";

int main(int argc, char* argv[]){
	if(argc < 2){
		std::cerr << "Not enough arguments\n";
		std::cerr << "Usage stocks [symbol]\n";
		return 1;
	}

	else {
		std::vector<std::string> args;						//Stores the symbols names provided by user
		std::vector<Stock> stocks;						//Will store classes for each ticker.
		unsigned count = 0;	
		size_t total_received = 0;
		size_t total_time = 0;
		for(int i = 1; i < argc; i++){
			if(i ==1 ){
				std::cout << "Symbol\t\tPrice\t\tChange\n";
			}
			std::string sym(argv[i]);
			Stock temp(sym);
			if(temp.GetCurrentPrice() < 0){
				continue;
			}

			else {
				std::cout << temp.GetSymbol() << "\t\t$" << temp.GetCurrentPrice() << "\t";
				if(temp.GetCurrentPrice() > temp.GetOpen()){
					std::cout << "\t" << GREEN << UP_ARROW;
				}
				if(temp.GetCurrentPrice() < temp.GetOpen()){
					std::cout << "\t" << RED << DOWN_ARROW;
				}

				std::cout << " $" << std::setprecision(2) << std::fixed << temp.GetCurrentPrice() - temp.GetOpen() << RESET <<std::endl;
				

			}
		}

	}

	return 0;
}

//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v10/finance/quoteSummmary/ /symbol?modules=price

//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v8/finance/chart/ /symbol?interval=2m

//Historical data CSV download
//https://query1.finance.yahoo.com/v7/finance/download + /symbol + ?period1= + /date1 + &period2= + /date2 + &interval= /interval + &events=history
