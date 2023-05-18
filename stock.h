#ifndef STOCK_H
#define STOCK_H

#include <string>
#include <curl/curl.h>

const std::string CURRENT_PRICE = "\"regularMarketPrice\":";
const std::string OPEN_PRICE = "\"open\":";
const std::string HIGH_PRICE = "\"high\":";
const std::string LOW_PRICE = "\"low\":";
const std::string VOLUME = "\"volume\":";



class Stock {
public:
	Stock(std::string);
	float GetCurrentPrice(){return m_current_price;};
	float GetOpen(){return m_open_price;};
	float GetHigh(){return m_high_price;};
	float GetLow(){return m_high_price;};
	std::string GetSymbol();
	long GetHTTPResCode(){return m_http_std_res_code;};
private:
	
	double m_current_price;
	double m_open_price;
	double m_high_price;
	double m_low_price;
	double m_volume;

	std::string m_symbol;
	std::string m_url;
	std::string m_website_data;
	long m_http_std_res_code;
private:
	std::string GenerateURL(std::string);
	void  GetWebsiteData();
	static size_t Callback(void*, size_t, size_t, void*);
	float ParsePrice(const std::string);
};

#endif
