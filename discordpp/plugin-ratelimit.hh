#pragma once

namespace discordpp{
	template<class BASE>
	class PluginRateLimit: public BASE, virtual BotStruct{
	public:
		void call(
				sptr<const std::string> requestType,
				sptr<const std::string> targetURL,
				sptr<const json> body,
				sptr<const std::function<void(const json)>> callback
		) override{
			std::cout << "here" << std::endl;
			BASE::call(requestType, targetURL, body, [callback](const json &msg){
				(*callback)(msg);
			});
		}
	};
}