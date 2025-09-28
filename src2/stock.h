// In stock.h
#ifndef STOCK_H
#define STOCK_H

#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <curl/curl.h>

// Keys to find in the JSON response
const std::string CURRENT_PRICE = "\"regularMarketPrice\":";
const std::string OPEN_PRICE = "\"regularMarketOpen\":"; // Corrected key for open price
const std::string HIGH_PRICE = "\"regularMarketDayHigh\":"; // Corrected key for day high
const std::string LOW_PRICE = "\"regularMarketDayLow\":"; // Corrected key for day low
const std::string VOLUME = "\"regularMarketVolume\":"; // Corrected key for volume

class Stock {
public:
	Stock(std::string);
	float GetCurrentPrice(){return m_current_price;};
	float GetOpen(){return m_open_price;};
	float GetHigh(){return m_high_price;};
	float GetLow(){return m_low_price;}; // <-- BUG FIX: Was returning m_high_price
	unsigned GetVolume(){return m_volume;};
	std::string GetSymbol();
	long GetHTTPResCode(){return m_http_std_res_code;};
	std::string GetRawData(){return m_website_data;};

private:
	double m_current_price;
	double m_open_price;
	double m_high_price;
	double m_low_price;
	unsigned m_volume;

	std::string m_symbol;
	std::string m_url;
	std::string m_website_data;
	long m_http_std_res_code;

private:
	std::string GenerateURL(std::string);
	bool GetWebsiteData();
	static size_t Callback(void*, size_t, size_t, void*);
	float ParseValue(const std::string&); // Renamed from ParsePrice
};

#endif
