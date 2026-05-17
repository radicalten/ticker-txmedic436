#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// User-Agent header to avoid rejection
void set_user_agent() {
    puts("User-Agent: MyTerminalApp/1.0");
}

// Simplified JSON parsing stub (replace with actual implementation)
StockData parse_yahoo_json(const char* url) {
    // Placeholder - replace with real parsing logic
    return {{"price": 150.75, "change": 0.03, "volume": 1000000.0}};
}

int main(int argc, char *argv[]) {
    set_user_agent();  // Ensures Yahoo API bypass
    StockData stock_data = parse_yahoo_json("https://query1.finance.yahoo.com/v2/finance/quote/AAPL/prices/market");

    printf("Stock Dashboard:\n");
    printf("Current Price: $%.2f\n", stock_data.price);
    printf("Change: %0.2f%%\n", stock_data.change);
    printf("Volume: %d\n", stock_data.volume);

    return 0;
}
