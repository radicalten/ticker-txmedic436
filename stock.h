#ifndef STOCK_H
#define STOCK_H

#include <string>
#include <curl/curl.h>

class Stock {
public:
	Stock(std::string);
	float GetPrice(){return m_current_price;};
	std::string GetSymbol();
	long GetHTTPResCode(){return m_http_std_res_code;};
private:
	double m_current_price;
	std::string m_symbol;
	std::string m_url;
	std::string m_website_data;
	long m_http_std_res_code;
private:
	std::string GenerateURL(std::string);
	void  GetWebsiteData();
	static size_t Callback(void*, size_t, size_t, void*);
	void ParsePrice();
};

#endif
