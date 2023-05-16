#include "stock.h"

Stock::Stock(std::string symbol, std::string data){
	m_symbol = symbol;

	std::string start_delimiter = "\"regularMarketPrice\":";
	char end_delimiter = ',';

	size_t start_pos = data.find(start_delimiter) + start_delimiter.length();
	size_t end_pos = data.find(end_delimiter, start_pos);

	std::string token = data.substr(start_pos, end_pos);
	m_current_price = std::stod(token.substr(0, token.find(end_delimiter)));
	
};
