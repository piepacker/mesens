#include "stdafx.h"
#include "MemoryManager.h"
#include "Console.h"
#include "BaseCartridge.h"
#include "Cpu.h"
#include "Ppu.h"
#include "Spc.h"
#include "RegisterHandlerA.h"
#include "RegisterHandlerB.h"
#include "RamHandler.h"
#include "MessageManager.h"
#include "DebugTypes.h"
#include "../Utilities/HexUtilities.h"

void MemoryManager::Initialize(shared_ptr<Console> console)
{
	_cyclesToRun = 0;
	_masterClock = 0;
	_console = console;
	_regs = console->GetInternalRegisters().get();
	_ppu = console->GetPpu();

	_workRam = new uint8_t[MemoryManager::WorkRamSize];

	_registerHandlerA.reset(new RegisterHandlerA(
		console->GetDmaController().get(),
		console->GetInternalRegisters().get(),
		console->GetControlManager().get()
	));
	
	_registerHandlerB.reset(new RegisterHandlerB(
		_console.get(),
		_ppu.get(),
		console->GetSpc().get(),
		_workRam
	));

	memset(_handlers, 0, sizeof(_handlers));
	//memset(_workRam, 0, 128 * 1024);

	for(uint32_t i = 0; i < 128 * 1024; i += 0x1000) {
		_workRamHandlers.push_back(unique_ptr<RamHandler>(new RamHandler(_workRam, i, SnesMemoryType::WorkRam)));
		RegisterHandler(0x7E0000 | i, 0x7E0000 | (i + 0xFFF), _workRamHandlers[_workRamHandlers.size() - 1].get());
	}

	for(int i = 0; i <= 0x3F; i++) {
		RegisterHandler((i << 16) | 0x2000, (i << 16) | 0x2FFF, _registerHandlerB.get());
		RegisterHandler(((i | 0x80) << 16) | 0x2000, ((i | 0x80) << 16) | 0x2FFF, _registerHandlerB.get());

		RegisterHandler((i << 16) | 0x4000, (i << 16) | 0x4FFF, _registerHandlerA.get());
		RegisterHandler(((i | 0x80) << 16) | 0x4000, ((i | 0x80) << 16) | 0x4FFF, _registerHandlerA.get());
	}

	for(int i = 0; i < 0x3F; i++) {
		RegisterHandler((i << 16) | 0x0000, (i << 16) | 0x0FFF, _workRamHandlers[0].get());
		RegisterHandler((i << 16) | 0x1000, (i << 16) | 0x1FFF, _workRamHandlers[1].get());
	}

	for(int i = 0x80; i < 0xBF; i++) {
		RegisterHandler((i << 16) | 0x0000, (i << 16) | 0x0FFF, _workRamHandlers[0].get());
		RegisterHandler((i << 16) | 0x1000, (i << 16) | 0x1FFF, _workRamHandlers[1].get());
	}

	console->GetCartridge()->RegisterHandlers(*this);

	GenerateMasterClockTable();
}

MemoryManager::~MemoryManager()
{
	delete[] _workRam;
}

void MemoryManager::RegisterHandler(uint32_t startAddr, uint32_t endAddr, IMemoryHandler * handler)
{
	if((startAddr & 0xFFF) != 0 || (endAddr & 0xFFF) != 0xFFF) {
		throw std::runtime_error("invalid start/end address");
	}

	for(uint32_t addr = startAddr; addr < endAddr; addr += 0x1000) {
		if(_handlers[addr >> 12]) {
			throw std::runtime_error("handler already set");
		}

		_handlers[addr >> 12] = handler;
	}
}

void MemoryManager::GenerateMasterClockTable()
{
	//This is incredibly inaccurate
	for(int j = 0; j < 2; j++) {
		for(int i = 0; i < 0x10000; i++) {
			uint8_t bank = (i & 0xFF00) >> 8;
			if(bank >= 0x40 && bank <= 0x7F) {
				//Slow
				_masterClockTable[j][i] = 8;
			} else if(bank >= 0xCF) {
				//Slow or fast (depending on register)
				_masterClockTable[j][i] = j == 1 ? 6 : 8;
			} else {
				uint8_t page = (i & 0xFF);
				if(page <= 0x1F) {
					//Slow
					_masterClockTable[j][i] = 8;
				} else if(page >= 0x20 && page <= 0x3F) {
					//Fast
					_masterClockTable[j][i] = 6;
				} else if(page == 0x40 || page == 0x41) {
					//Extra slow
					_masterClockTable[j][i] = 12;
				} else if(page >= 0x42 && page <= 0x5F) {
					//Fast
					_masterClockTable[j][i] = 6;
				} else if(page >= 0x60 && page <= 0x7F) {
					//Slow
					_masterClockTable[j][i] = 8;
				} else {
					//page >= $80
					//Slow or fast (depending on register)
					_masterClockTable[j][i] = j == 1 ? 6 : 8;
				}
			}
		}
	}
}

void MemoryManager::IncrementMasterClock(uint32_t addr)
{
	IncrementMasterClockValue(_masterClockTable[(uint8_t)_regs->IsFastRomEnabled()][addr >> 8]);
}

void MemoryManager::IncrementMasterClockValue(uint16_t value)
{
	_masterClock += value;
	_cyclesToRun += value;

	if(_cyclesToRun >= 12) {
		_cyclesToRun -= 12;
		_ppu->Exec();
		_ppu->Exec();
		_ppu->Exec();
	} else if(_cyclesToRun >= 8) {
		_cyclesToRun -= 8;
		_ppu->Exec();
		_ppu->Exec();
	} else if(_cyclesToRun >= 4) {
		_cyclesToRun -= 4;
		_ppu->Exec();
	}
}

uint8_t MemoryManager::Read(uint32_t addr, MemoryOperationType type)
{
	IncrementMasterClock(addr);

	uint8_t value;
	if(_handlers[addr >> 12]) {
		value = _handlers[addr >> 12]->Read(addr);
	} else {
		//open bus
		value = (addr >> 12);

		MessageManager::DisplayMessage("Debug", "Read - missing handler: $" + HexUtilities::ToHex(addr));
	}
	_console->ProcessCpuRead(addr, value, type);
	return value;
}

uint8_t MemoryManager::ReadDma(uint32_t addr)
{
	IncrementMasterClockValue<4>();
	uint8_t value;
	if(_handlers[addr >> 12]) {
		value = _handlers[addr >> 12]->Read(addr);
	} else {
		//open bus
		value = (addr >> 12);
		MessageManager::DisplayMessage("Debug", "Read - missing handler: $" + HexUtilities::ToHex(addr));
	}
	_console->ProcessCpuRead(addr, value, MemoryOperationType::DmaRead);
	return value;
}

uint8_t MemoryManager::Peek(uint32_t addr)
{
	//Read, without triggering side-effects
	uint8_t value = 0;
	if(_handlers[addr >> 12]) {
		value = _handlers[addr >> 12]->Peek(addr);
	}
	return value;
}

uint16_t MemoryManager::PeekWord(uint32_t addr)
{
	uint8_t lsb = Peek(addr);
	uint8_t msb = Peek((addr + 1) & 0xFFFFFF);
	return (msb << 8) | lsb;
}

void MemoryManager::Write(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	IncrementMasterClock(addr);

	_console->ProcessCpuWrite(addr, value, type);
	if(_handlers[addr >> 12]) {
		return _handlers[addr >> 12]->Write(addr, value);
	} else {
		MessageManager::DisplayMessage("Debug", "Write - missing handler: $" + HexUtilities::ToHex(addr) + " = " + HexUtilities::ToHex(value));
	}
}

void MemoryManager::WriteDma(uint32_t addr, uint8_t value)
{
	IncrementMasterClockValue<4>();

	_console->ProcessCpuWrite(addr, value, MemoryOperationType::DmaWrite);
	if(_handlers[addr >> 12]) {
		return _handlers[addr >> 12]->Write(addr, value);
	} else {
		MessageManager::DisplayMessage("Debug", "Write - missing handler: $" + HexUtilities::ToHex(addr) + " = " + HexUtilities::ToHex(value));
	}
}

uint64_t MemoryManager::GetMasterClock()
{
	return _masterClock;
}

uint8_t * MemoryManager::DebugGetWorkRam()
{
	return _workRam;
}

AddressInfo MemoryManager::GetAbsoluteAddress(uint32_t addr)
{
	if(_handlers[addr >> 12]) {
		return _handlers[addr >> 12]->GetAbsoluteAddress(addr);
	} else {
		return { -1, SnesMemoryType::CpuMemory };
	}
}

int MemoryManager::GetRelativeAddress(AddressInfo &address, int32_t cpuAddress)
{
	uint16_t startPosition;
	if(cpuAddress < 0) {
		uint8_t bank = _console->GetCpu()->GetState().K;
		startPosition = ((bank & 0xC0) << 4);
	} else {
		startPosition = (cpuAddress >> 12) & 0xF00;
	}

	for(int i = startPosition; i <= 0xFFF; i++) {
		if(_handlers[i]) {
			AddressInfo addrInfo = _handlers[i]->GetAbsoluteAddress(address.Address & 0xFFF);
			if(addrInfo.Type == address.Type && addrInfo.Address == address.Address) {
				return (i << 12) | (address.Address & 0xFFF);
			}
		}
	}
	for(int i = 0; i < startPosition; i++) {
		if(_handlers[i]) {
			AddressInfo addrInfo = _handlers[i]->GetAbsoluteAddress(address.Address & 0xFFF);
			if(addrInfo.Type == address.Type && addrInfo.Address == address.Address) {
				return (i << 12) | (address.Address & 0xFFF);
			}
		}
	}
	return -1;
}