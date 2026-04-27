#include "c74_min.h"

#include <string>
#include <vector>

using namespace c74::min;

static std::string to_string(const symbol &s) {
	return std::string(s.c_str());
}

class bbb_webrtc_cfg : public object<bbb_webrtc_cfg> {
public:
	MIN_DESCRIPTION{"WebRTC ICE server configuration helper"};
	MIN_TAGS{"webrtc, config"};
	MIN_AUTHOR{"ISHII 2bit"};

	inlet<> input{this, "(messages) configuration input"};
	outlet<> output{this, "(anything) formatted ICE config output"};

	attribute<symbol> stun_server{this, "stun_server", "stun:stun.l.google.com:19302",
		description{"STUN server URL"}
	};

	attribute<symbol> turn_server{this, "turn_server", "",
		description{"TURN server URL"}
	};

	attribute<symbol> turn_username{this, "turn_username", "",
		description{"TURN username"}
	};

	attribute<symbol> turn_password{this, "turn_password", "",
		description{"TURN password"}
	};

	message<> bang_msg{this, "bang", "Output current configuration",
		MIN_FUNCTION {
			output_config();
			return {};
		}
	};

	message<> dump_msg{this, "dump", "Print current config",
		MIN_FUNCTION {
			cout << "bbb.webrtc.cfg:" << endl;
			cout << "  stun: " << to_string(stun_server) << endl;
			auto turn = to_string(turn_server);
			if(!turn.empty()) {
				cout << "  turn: " << turn << endl;
				cout << "  username: " << to_string(turn_username) << endl;
			} else {
				cout << "  turn: (none)" << endl;
			}
			return {};
		}
	};

	message<> defaults_msg{this, "defaults", "Reset to default STUN server",
		MIN_FUNCTION {
			stun_server = symbol("stun:stun.l.google.com:19302");
			turn_server = symbol("");
			turn_username = symbol("");
			turn_password = symbol("");
			return {};
		}
	};

private:
	void output_config() {
		atoms stun_args;
		stun_args.push_back(atom(symbol("stun_server")));
		stun_args.push_back(atom(stun_server));
		output.send(stun_args);

		auto turn = to_string(turn_server);
		if(!turn.empty()) {
			atoms turn_args;
			turn_args.push_back(atom(symbol("turn_server")));
			turn_args.push_back(atom(turn_server));
			output.send(turn_args);

			atoms user_args;
			user_args.push_back(atom(symbol("turn_username")));
			user_args.push_back(atom(turn_username));
			output.send(user_args);

			atoms pass_args;
			pass_args.push_back(atom(symbol("turn_password")));
			pass_args.push_back(atom(turn_password));
			output.send(pass_args);
		}
	}
};

MIN_EXTERNAL(bbb_webrtc_cfg);
