//Stock Ticker - Provides stock prices based on symbols provided from arguments
//Copyright(C) 2023 TxMedic435

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <curl/curl.h>
#include "stock.h"
#include "options.h"

//COLORED OUTPUT 
#define RED "\033[31m"
#define GREEN "\033[32m"
#define RESET "\033[0m"

//UNICODE 
const std::string UP_ARROW = "\u2191";
const std::string DOWN_ARROW = "\u2193";

int main(int argc, char* argv[]){
	std::vector<Stock> stocks;							//Will store classes for each ticker symbol.
	
	if(argc < 2){
		std::cerr << "Not enough arguments\n";
		PrintHelp();
		return 1;
	}

	else {
		std::vector<std::string> args;						//Stores the symbols names provided by user
		
		//Parse each symbol from arguments and create a Stock instance of it.
		for(int i = 1; i < argc; i++){
			if(i ==1 ){
				std::cout << "Symbol\t\tPrice\t\tChange\t\tChange(%)\n";
			}
			std::string sym(argv[i]);
			Stock temp(sym);
			if(temp.GetCurrentPrice() < 0){					//Ignore invalid symbols
				continue;
			}

			else {
				//Print symbol and current price
				std::cout << temp.GetSymbol() << "\t\t$" << std::setprecision(2) << std::fixed << temp.GetCurrentPrice() << "\t\t";
				if(temp.GetCurrentPrice() > temp.GetOpen()){
					std::cout << GREEN << UP_ARROW;
				}
				if(temp.GetCurrentPrice() < temp.GetOpen()){
					std::cout <<  RED << DOWN_ARROW;
				}
				
				//Print change in dollar amount
				std::cout << " $" << std::setprecision(2) << std::fixed << temp.GetCurrentPrice() - temp.GetOpen() << RESET <<"\t";
				
				//Print change in percentage
				float percent = 100 * (temp.GetCurrentPrice() - temp.GetOpen()) / temp.GetOpen();
				if(percent < 0){
					std::cout << RED;
				}
				if(percent > 0){
					std::cout << "\t" << GREEN;
				}
				std::cout << std::setprecision(2) << std::fixed << percent << "%" << RESET << std::endl;

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
