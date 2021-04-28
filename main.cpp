#include <chrono>
#include <thread>
#include <iostream>
#include <signal.h>
#include <utility>
#include <cstring>
#include <poll.h>
#include <fstream>
#include <map>
#include <filesystem>
#include <sys/wait.h>
#include "MCL/strmanip.h"
#include "MCL/ttymanip.h"

namespace fs = std::filesystem;



#define WRITE_LITERAL(f, s) write(f, s, sizeof(s))
#define CURRENT_TIME std::chrono::steady_clock::now()


void onInterrupt(int sig)
{
	WRITE_LITERAL(0, AEC_RESET AEC_ERASE_SE AEC_SHOW_CURSOR);
	mcl::tty term{};
	term.cook();
	exit(0);
}


class playlist
{
public:
	struct inst
	{
		enum e {
			play,
			start,
			end
		} m_e;
		std::vector<std::string> args;
	};

private:
	std::map<std::string, std::string> paths = {};
	std::vector<inst> ins = {};
	int isp = 0;

	static int comms[2];
	std::vector<std::pair<std::string, int>> playing = {};


	int playFile(std::string f)
	{
		int pid = fork();
		switch (pid)
		{
		case -1:
			throw std::runtime_error("Failed to fork.");
		case 0: //Am child
			execlp("play", "play", "-q", f.c_str(), 0);
		}
		return pid;
	}


public:
	playlist(const std::string& filename)
	{
		if (!comms[0])
			pipe(comms);
		std::ifstream f{filename};
		if (!f)
			throw std::runtime_error{
				"Failed to open \"" + filename + "\""
			};

		fs::current_path(fs::path(filename).parent_path());

		std::string errstr{""};
		for (std::string line; getline(f, line), f; )
		{
			if (mcl::isWhite(line) == line.size() || mcl::trim(line)[0] == '#')
				continue;
			auto tok = mcl::tokens(line);

			if (tok[0] == "name")
			{
				if (tok.size() & 1)
				{
					for (int i = 1; i < tok.size(); i += 2)
					{
						if (fs::is_regular_file(fs::path(tok[i])))
							paths[tok[i+1]] = tok[i];
						else
							errstr += "\"" + tok[i] + "\" does not name a normal file\n";
					}
				}
				else
					errstr += "`name` takes path-name pairs\n";
			}
			else if (tok[0] == "play")
			{
				ins.push_back({inst::play, {}});
				for (int i = 1; i < tok.size(); i++)
				{
					if (paths.count(tok[i]))
						ins.back().args.push_back(tok[i]);
					else
						errstr += "Name \"" + tok[i] + "\" has not been defined\n";
				}
			}
			else if (tok[0] == "start")
			{
				ins.push_back({inst::start, {}});
				for (int i = 1; i < tok.size(); i++)
				{
					if (paths.count(tok[i]))
						ins.back().args.push_back(tok[i]);
					else
						errstr += "Name \"" + tok[i] + "\" has not been defined\n";
				}
			}
			else if (tok[0] == "end")
			{
				ins.push_back({inst::end, {}});
				for (int i = 1; i < tok.size(); i++)
				{
					if (paths.count(tok[i]))
						ins.back().args.push_back(tok[i]);
					else
						errstr += "Name \"" + tok[i] + "\" has not been defined\n";
				}
			}
			else
				errstr += "Unrecognized instruction `" + tok[0] + "`\n";
		}

		if (errstr.size())
			throw std::domain_error(errstr);
	}


	void step()
	{
		switch (ins[isp].m_e)
		{
		case inst::play:
		case inst::start:
			for (const std::string& s : ins[isp].args)
				playing.push_back({s, playFile(paths[s])});
			break;
		case inst::end:
			for (const std::string& s : ins[isp].args)
			for (auto& p : playing)
			{
				if (p.second && p.first == s)
				{
					kill(p.second, SIGTERM);
					break;
				}
			}
		}
		++isp;
	}
	void stepBack()
	{
		if (isp)
			--isp;
	}
	void skip()
	{
		++isp;
	}


	std::vector<std::pair<std::string, int>>&
	getPlaying()
	{
		pollfd* pll = new pollfd{comms[0], POLLIN, 0};
		for (int pid; poll(pll, 1, 0); )
		{
			read(comms[0], &pid, sizeof(pid));
			for (auto& p : playing)
			{
				if (p.second == pid)
				{
					p.second = 0;
					break;
				}
			}
		}

		return playing;
	}

	std::vector<inst> getUpcoming()
	{
		return std::vector<inst>{ins.begin()+isp, ins.end()};
	}

	const std::vector<inst>& getInstructions() const
	{
		return ins;
	}

	std::vector<std::string> registeredNames() const
	{
		std::vector<std::string> out(paths.size());
		for (auto& m : paths)
			out.push_back(m.first);
		return out;
	}

	int getISP() const
	{
		return isp;
	}

	bool done() const
	{
		return isp == ins.size();
	}

	static void onChildDeath(int sig)
	{
		int status;
		int pid = wait(&status);
		write(comms[1], &pid, sizeof(pid));
	}
};

int playlist::comms[2] = { 0, 0 };


int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "Usage: " << argv[0] << " <playlist>\n";
		return 1;
	}

	playlist pl{argv[1]};

	size_t columnWidth = 0;
	for (auto& s : pl.registeredNames())
	{
		if (columnWidth < s.size()+3)
			columnWidth += 8;
	}

	struct sigaction sa;
	sa.sa_handler = onInterrupt;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = pl.onChildDeath;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);

	using namespace std::literals;
	mcl::tty term{};
	term.uncook();
	term.aquireSize();
	const auto startpos = term.getPos();
	std::cout << AEC_CSI "?25l";



	pollfd* stdinPoll = new pollfd{0, POLLIN, 0};

	std::stringstream msgbuf{""};


	const auto tick = 50ms; //20tps = tick / (50ms)
	auto next = std::chrono::steady_clock::now();
	for (bool running = 1; running;)
	{
		//Make sure loop starts steadily, no matter how long each particular
		//goround takes.
		std::this_thread::sleep_until(next += tick);

		int lines = 0;
		msgbuf << AEC_ERASE_SE;
		running = !pl.done();
		for (auto& p : pl.getPlaying())
		{
			if (p.second > 0)
			{
				msgbuf << AEC_GREEN_FG << p.first << AEC_RESET "\n";
				++lines;
				running = 1;
			}
			else if (p.second > -30)
			{
				msgbuf << AEC_RED_FG << p.first << AEC_RESET "\n";
				--p.second;
				++lines;
				running = 1;
			}
		}


		msgbuf << mcl::aec::up(lines);
		lines = 0;
		
		for (auto i = pl.getInstructions().begin();
			i != pl.getInstructions().end(); i++)
		{
			msgbuf << mcl::aec::forward(columnWidth);
			if (i == pl.getISP()+pl.getInstructions().begin())
			{
				msgbuf << "> "
					<< std::string("end ", 0, 4 * (i->m_e == playlist::inst::end))
					<< AEC_YELLOW_FG;
				for (auto& a : i->args)
					msgbuf << a << ' ';
				msgbuf << AEC_RESET "<\n";
			}
			else
			{
				msgbuf <<
					(i->m_e == playlist::inst::start ? AEC_BLUE_FG :
					 i->m_e == playlist::inst::play  ? AEC_CYAN_FG :
					                                   AEC_BRIGHT_RED_FG);
				for (auto& a : i->args)
					msgbuf << a << ' ';
				msgbuf << AEC_RESET "\n";
			}
			++lines;
		}
		std::cout << msgbuf.str() << mcl::aec::up(lines) << std::flush;
		msgbuf.str("");

		/**
		 * Task list:
		 * - Poll && Recieve user input
		 * x Check status of running sounds (handled by playlist)
		 */


		if (poll(stdinPoll, 1, 0))
		{
			switch (getchar())
			{
			case '\n':
			case ' ':
				if (!pl.done())
					pl.step();
				break;
			case '\b':
			case 'b':
				pl.stepBack();
				break;
			case 's':
				pl.skip();
			default:
				break;
			}
		}

	}

	onInterrupt(SIGINT);

	return 0;
}