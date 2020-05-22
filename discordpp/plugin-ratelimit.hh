#pragma once

namespace discordpp{
	template<class BASE>
	class PluginRateLimit: public BASE, virtual BotStruct{
	public:
		virtual void call(
				sptr<const std::string> requestType,
				sptr<const std::string> targetURL,
				sptr<const json> body,
				sptr<const std::function<void()>> on_write,
				sptr<const std::function<void(const json)>> on_read
		) override{
			auto id = ids.find(*targetURL);
			BASE::call(
					requestType, targetURL, body,
					std::make_shared<std::function<void()>>(
							[on_write](){
								if(on_write != nullptr){
									(*on_write)();
								}
								std::cout << "on_write" << std::endl;
							}
					),
					std::make_shared<std::function<void(const json)>>(
							[on_read](const json &msg){
								std::cout << "on_read" << std::endl;
								if(on_read != nullptr){
									(*on_read)(msg);
								}
							}
					)
			);
		}
	};
}