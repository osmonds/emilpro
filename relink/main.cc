#include <model.hh>
#include <emilpro.hh>
#include <utils.hh>
#include <swap-endian.hh>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>

#include <elf.h>

#include <functional>
#include <unordered_map>

using namespace emilpro;

static void walkDir(const std::string& dirName, std::function<void(const std::string &)> onFile)
{
	struct dirent *de;
	DIR *dir;

	dir = ::opendir(dirName.c_str());
	if (!dir)
		return;

	for (de = readdir(dir);
			de;
			de = readdir(dir)) {
		struct stat st;

		std::string name(de->d_name);
		std::string curPath = dirName + "/" + name;

		if (name == "." || name == "..")
			continue;


		lstat(std::string(dirName + "/" + name).c_str(), &st);
		// Recuse and check the next directory level
		if (S_ISREG(st.st_mode)) {
			onFile(curPath);
			continue;
		} else if (S_ISDIR(st.st_mode))
			walkDir(curPath, onFile);
	}

	::closedir(dir);

	return;
}

static void *checkElf(const std::string &path, unsigned int e_type, size_t &outSz)
{
	size_t sz;
	Elf32_Ehdr *ehdr = (Elf32_Ehdr *)peek_file(&sz, "%s", path.c_str());

	if (!ehdr)
		return nullptr;

	if (sz < sizeof(Elf32_Ehdr))
		return nullptr;

	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
		return nullptr;

	uint32_t hdr_e_type = le_to_host<uint16_t>(ehdr->e_type);

	if (ehdr->e_ident[EI_DATA] == ELFDATA2MSB)
		hdr_e_type = be_to_host<uint16_t>(ehdr->e_type);

	if (hdr_e_type != e_type)
		return nullptr;

	return read_file(&outSz, "%s", path.c_str());
}

std::unordered_map<std::string, unsigned int> binarySymbols;
std::unordered_map<std::string, unsigned int> keepSymbols;
unsigned long long totalSyms, keepSize, symSize;

static void parseFile(const std::string &path)
{
	size_t sz;
	void *p = checkElf(path, ET_EXEC, sz);

	if (!p)
		return;

	// HACK!
	if (sz > 10 * 1024 * 1024) {
		return;
		free(p);
	}

	printf("EXECUTABLE: %-45s: ", path.c_str());
	EmilPro::destroy();
	auto &model = Model::instance();

	model.addData(p, sz);
	model.parseAll();
	while (!model.parsingComplete())
		;

	auto syms = model.getSymbols();
	unsigned int undefs = 0;
	for (auto &it : syms) {
		const ISymbol *cur = it;

		if (cur->getLinkage() == ISymbol::LINK_UNDEFINED)
		{
			binarySymbols[cur->getName()]++;
			undefs++;
		}
	}
	printf("%u undefined\n", undefs);

	free(p);
}

static void parseSolibs(const std::string &path)
{
	size_t sz;
	void *p = checkElf(path, ET_DYN, sz);

	if (!p)
		return;

	printf("LIBRARY:    %-45s: ", path.c_str());
	EmilPro::destroy();
	auto &model = Model::instance();

	model.addData(p, sz);
	model.parseAll();
	while (!model.parsingComplete())
		;

	auto syms = model.getSymbols();
	auto relocs = model.getRelocations();

	unsigned int keeps = keepSymbols.size();
	for (auto &it : syms) {
		const ISymbol *cur = it;

		totalSyms++;

		// Keep all non-code symbols
		if (cur->getType() != ISymbol::SYM_TEXT) {
			keepSymbols[cur->getName()]++;

			continue;
		}
		symSize += cur->getSize();

		auto sec = model.getSection(cur->getAddress());
		if (!sec)
			continue;

		// Keep everything outside the regular text segment
		if (sec->getName() != ".text") {
			keepSymbols[cur->getName()]++;
			keepSize += cur->getSize();

			continue;
		}


		// Used by the binary or other solibs
		if (cur->getLinkage() == ISymbol::LINK_DYNAMIC &&
				binarySymbols.find(cur->getName()) != binarySymbols.end()) {
			keepSymbols[cur->getName()]++;
			keepSize += cur->getSize();
		}
	}
	keeps = keepSymbols.size() - keeps;

	for (auto &it : relocs) {
		const auto &sym = it->getTargetSymbol();

		auto sec = model.getSection(it->getSourceAddress());

		printf("XXX: 0x%llx and %s is in %s\n", it->getSourceAddress(), sym.getName().c_str(), sec ? sec->getName().c_str() : "huh?");
	}

	printf("%u keep\n", keeps);
	free(p);
}

static void zeroSolibUnused(const std::string &path)
{
	size_t sz;
	void *p = checkElf(path, ET_DYN, sz);
	uint8_t *bp = (uint8_t*)p;

	if (!p)
		return;
	EmilPro::destroy();
	auto &model = Model::instance();

	model.addData(p, sz);
	model.parseAll();
	while (!model.parsingComplete())
		;

	auto syms = model.getSymbols();
	unsigned int keeps = 0;

	for (auto &it : syms) {
		const ISymbol *cur = it;

		if (cur->getType() != ISymbol::SYM_TEXT)
		    continue;

		if (keepSymbols.find(cur->getName()) != keepSymbols.end())
		    continue;

		memset(bp + cur->getFileOffset(), 0, cur->getSize());
	}

	write_file(p, sz, "%s", path.c_str());

	free(p);
}

int main(int argc, const char *argv[])
{
	if (argc < 2)
		return 0;

	EmilPro::init();

	walkDir(argv[1], parseFile);
	walkDir(argv[1], parseSolibs);
//	walkDir(argv[1], zeroSolibUnused);

	printf("Will keep %lld / %lld symbols (zero %lld KB, keep %lld KB)\n",
			(unsigned long long)keepSymbols.size(), totalSyms,
			(symSize - keepSize) / 1024,
			keepSize / 1024);
	for (auto &it : keepSymbols) {
//		printf("Will keep %s: %d\n", it.first.c_str(), it.second);
	}

	return 0;
}
