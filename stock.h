#ifndef STOCK_H
#define STOCK_H

#include <string>

class Stock {
public:
	Stock(std::string, std::string);
	float GetPrice(){return m_current_price;};
private:
	float m_current_price;
	std::string m_symbol;
};

#endif
