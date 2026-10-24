#include <atomic>
#include <string>
#include <vector>

struct ParsedUrl;

class PlayerbotLLMInterface
{
public:
    PlayerbotLLMInterface() {}
    static std::string SanitizeForJson(const std::string& input);

    // Sends body as an HTTP/1.1 POST with Connection: close and reads until the server closes the connection
    // or timeOutSeconds pass. response gets the raw answer, headers included. Returns false when it cannot
    // connect or send. Blocks the calling thread, so never call it from the world thread.
    static bool HttpPost(const ParsedUrl& url, const std::string& body, int timeOutSeconds, const std::string& bearerToken, std::string& response, std::vector<std::string>& debugLines);

    static std::string Generate(const std::string& prompt, int timeOutSeconds, int maxGenerations, std::vector<std::string>& debugLines);

    static std::vector<std::string> ParseResponse(const std::string& response, const std::string& startPattern, const std::string& endPattern, const std::string& deletePattern, const std::string& splitPattern, std::vector<std::string>& debugLines);

    static void LimitContext(std::string& context, int currentLength);
private:
    std::atomic<int> generationCount = 0;
};

#define sPlayerbotLLMInterface MaNGOS::Singleton<PlayerbotLLMInterface>::Instance()

