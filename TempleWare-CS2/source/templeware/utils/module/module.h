#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Class names might be too broad ngl -- just namespace them if someones ever really cares
class Module {
public:
	Module(uintptr_t moduleAddress, const std::string &moduleName);

	uintptr_t address;
	std::string name;
};

class Modules {
public:
	void init();

	uintptr_t getModule(const std::string &moduleName);
private:
	// example usage: registerModule("client.dll", "client")
	bool registerModule(const std::string &aModuleName, const std::string &moduleName);

	std::vector<Module> modules;
};

// We luv global state
extern Modules modules;