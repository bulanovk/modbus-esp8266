/*
    Modbus.cpp - Modbus Base Library Implementation
    Copyright (C) 2014 Andr� Sarmento Barbosa
                  2017 Alexander Emelianov (a.m.emelianov@gmail.com)
*/
#include "Modbus.h"

uint16_t cbDefault(TRegister* reg, uint16_t val) {
	return val;
}

TRegister* Modbus::searchRegister(uint16_t address) {
    const TRegister tmp = {address, 0, cbDefault, cbDefault};
    std::list<TRegister>::iterator it = std::find(_regs.begin(), _regs.end(), tmp);
    //std::vector<TRegister>::iterator it = std::find(_regs.begin(), _regs.end(), tmp);
    if (it != _regs.end()) return &*it;
    return NULL;
}

bool Modbus::addReg(uint16_t address, uint16_t value, uint16_t numregs) {
   #ifdef MB_MAX_REGS
    if (_regs.size() + numregs > MB_MAX_REGS) return false;
   #endif
    for (uint16_t i = 0; i < numregs; i++) {
        if (!searchRegister(address + i))
            _regs.push_back({address + i, value, cbDefault, cbDefault});
    }
    _regs.sort();
    //std::sort(_regs.begin(),_regs.end());
    //_regs.unique();
    //std::vector<TRegister>::iterator it;
    //it = std::unique (_regs.begin(), _regs.end());
    //_regs.resize( std::distance(_regs.begin(),it) );
    return true;
}

bool Modbus::Reg(uint16_t address, uint16_t value) {
    TRegister* reg;
    //search for the register address
    reg = this->searchRegister(address);
    //if found then assign the register value to the new value.
    if (reg) {
        if (cbEnabled) {
            reg->value = reg->set(reg, value);
        } else {
            reg->value = value;
        }
        return true;
    } else 
        return false;
}

uint16_t Modbus::Reg(uint16_t address) {
    TRegister* reg;
    reg = this->searchRegister(address);
    if(reg)
        if (cbEnabled) {
            return reg->get(reg, reg->value);
        } else {
            return reg->value;
        }
    else
        return 0;
}

void Modbus::receivePDU(uint8_t* frame) {
    FunctionCode fcode  = (FunctionCode)frame[0];
    uint16_t field1 = (uint16_t)frame[1] << 8 | (uint16_t)frame[2];
    uint16_t field2 = (uint16_t)frame[3] << 8 | (uint16_t)frame[4];
    uint16_t bytecount_calc;
    switch (fcode) {

        case FC_WRITE_REG:
            //field1 = reg, field2 = value
            this->writeSingleRegister(field1, field2);
        break;

        case FC_READ_REGS:
            //field1 = startreg, field2 = numregs
            this->readRegisters(field1, field2);
        break;

        case FC_WRITE_REGS:
            //field1 = startreg, field2 = status
            this->writeMultipleRegisters(frame,field1, field2, frame[5]);
        break;

        case FC_READ_COILS:
            //field1 = startreg, field2 = numregs
            this->readCoils(field1, field2);
        break;

        case FC_READ_INPUT_STAT:
            //field1 = startreg, field2 = numregs
            this->readInputStatus(field1, field2);
        break;

        case FC_READ_INPUT_REGS:
            //field1 = startreg, field2 = numregs
            this->readInputRegisters(field1, field2);
        break;

        case FC_WRITE_COIL:
            //field1 = reg, field2 = status
            //this->writeSingleCoil(field1, field2);
            //Check value (status)
            if (field2 != 0xFF00 && field2 != 0x0000) {
                this->exceptionResponse(fcode, EX_ILLEGAL_VALUE);
                return;
            }

            //Check Address and execute (reg exists?)
            if (!this->Coil(field1, (bool)field2)) {
                this->exceptionResponse(fcode, EX_ILLEGAL_ADDRESS);
                return;
            }

            //Check for failure
            if (this->Coil(field1) != (bool)field2) {
                this->exceptionResponse(fcode, EX_SLAVE_FAILURE);
                return;
            }

            _reply = REPLY_ECHO;
        break;

        case FC_WRITE_COILS:
        //field1 = startreg, field2 = numoutputs, frame[5] = bytecount
            bytecount_calc = field2 / 8;
            if (field2%8) bytecount_calc++;
            if (field2 < 0x0001 || field2 > 0x07B0 || frame[5] != bytecount_calc) {
                this->exceptionResponse(fcode, EX_ILLEGAL_VALUE);
                return;
            }
            Serial.println("Size ok");
            //Check Address (startreg...startreg + numregs)
            for (int k = 0; k < field2; k++) {
                if (!this->searchRegister(COIL(field1) + k)) {
                    this->exceptionResponse(fcode, EX_ILLEGAL_ADDRESS);
                    return;
                }
            }
            this->writeMultipleCoils(frame + MB_FRAME_HEADER, field1, field2, frame[5]);
            successResponce(field1, field2, FC_WRITE_COILS);
            _reply = REPLY_NORMAL;
        break;

        default:
            this->exceptionResponse(fcode, EX_ILLEGAL_FUNCTION);
    }
}

void Modbus::exceptionResponse(FunctionCode fn, ResultCode excode) {
    //Clean frame buffer
    free(_frame);
    _len = 2;
    _frame = (uint8_t*) malloc(_len);
    _frame[0] = fn + 0x80;
    _frame[1] = excode;

    _reply = REPLY_NORMAL;
}

void Modbus::readBits(uint16_t startreg, uint16_t numregs, FunctionCode fn) {
    //Check value (numregs)
    if (numregs < 0x0001 || numregs > 0x07D0) {
        this->exceptionResponse(fn, EX_ILLEGAL_VALUE);
        return;
    }

    //Check Address
    //Check only startreg. Is this correct?
    //When I check all registers in range I got errors in ScadaBR
    //I think that ScadaBR request more than one in the single request
    //when you have more then one datapoint configured from same type.
    if (!this->searchRegister(startreg)) {
        this->exceptionResponse(fn, EX_ILLEGAL_ADDRESS);
        return;
    }

    //Clean frame buffer
    free(_frame);

    //Determine the message length = function type, byte count and
	//for each group of 8 registers the message length increases by 1
	_len = 2 + numregs/8;
	if (numregs%8) _len++; //Add 1 to the message length for the partial byte.

    _frame = (uint8_t*) malloc(_len);
    if (!_frame) {
        this->exceptionResponse(fn, EX_SLAVE_FAILURE);
        return;
    }
    _frame[0] = fn;
    _frame[1] = _len - 2; //byte count (_len - function code and byte count)
	_frame[_len - 1] = 0;  //Clean last probably partial byte

    uint8_t bitn = 0;
    uint16_t totregs = numregs;
    uint16_t i;
	while (numregs--) {
        i = (totregs - numregs) / 8;
		if (BIT_BOOL(this->Reg(startreg)))
			bitSet(_frame[2+i], bitn);
		else
			bitClear(_frame[2+i], bitn);
		//increment the bit index
		bitn++;
		if (bitn == 8) bitn = 0;
		//increment the register
		startreg++;
	}

    _reply = REPLY_NORMAL;
}

void Modbus::readWords(uint16_t startreg, uint16_t numregs, FunctionCode fn) {
    //Check value (numregs)
    if (numregs < 0x0001 || numregs > 0x007D) {
        this->exceptionResponse(fn, EX_ILLEGAL_VALUE);
        return;
    }

    //Check Address
    //*** See comments on readCoils method.
    if (!this->searchRegister(startreg)) {
        this->exceptionResponse(fn, EX_ILLEGAL_ADDRESS);
        return;
    }

    //Clean frame buffer
    free(_frame);

	//calculate the query reply message length
	//for each register queried add 2 bytes
	_len = 2 + numregs * 2;

    _frame = (uint8_t*) malloc(_len);
    if (!_frame) {
        this->exceptionResponse(fn, EX_SLAVE_FAILURE);
        return;
    }

    _frame[0] = fn;
    _frame[1] = _len - 2;   //byte count

    uint16_t val;
    uint16_t i = 0;
	while(numregs--) {
        //retrieve the value from the register bank for the current register
        val = this->Reg(startreg + i);
        //write the high byte of the register value
        _frame[2 + i * 2]  = val >> 8;
        //write the low byte of the register value
        _frame[3 + i * 2] = val & 0xFF;
        i++;
	}

    _reply = REPLY_NORMAL;
}

void Modbus::writeSingleRegister(uint16_t reg, uint16_t value, FunctionCode fn) {
    //No necessary verify illegal value (EX_ILLEGAL_VALUE) - because using uint16_t (0x0000 - 0x0FFFF)
    //Check Address and execute (reg exists?)
    if (!this->Hreg(reg, value)) {
        this->exceptionResponse(fn, EX_ILLEGAL_ADDRESS);
        return;
    }

    //Check for failure
    if (this->Hreg(reg) != value) {
        this->exceptionResponse(fn, EX_SLAVE_FAILURE);
        return;
    }

    _reply = REPLY_ECHO;
}

void Modbus::writeMultipleRegisters(uint8_t* frame,uint16_t startreg, uint16_t numoutputs, uint8_t bytecount, FunctionCode fn) {
    //Check value
    if (numoutputs < 0x0001 || numoutputs > 0x007B || bytecount != 2 * numoutputs) {
        this->exceptionResponse(fn, EX_ILLEGAL_VALUE);
        return;
    }

    //Check Address (startreg...startreg + numregs)
    for (int k = 0; k < numoutputs; k++) {
        if (!this->searchRegister(HREG(startreg) + k)) {
            this->exceptionResponse(fn, EX_ILLEGAL_ADDRESS);
            return;
        }
    }

    //Clean frame buffer
    free(_frame);
	_len = 5;
    _frame = (uint8_t*) malloc(_len);

    _frame[0] = fn;
    _frame[1] = startreg >> 8;
    _frame[2] = startreg & 0x00FF;
    _frame[3] = numoutputs >> 8;
    _frame[4] = numoutputs & 0x00FF;

    uint16_t val;
    uint16_t i = 0;
	while(numoutputs--) {
        val = (word)frame[6+i*2] << 8 | (word)frame[7+i*2];
        this->Hreg(startreg + i, val);
        i++;
	}

    _reply = REPLY_NORMAL;
}

void Modbus::writeSingleCoil(uint16_t reg, uint16_t status, FunctionCode fn) {
    //Check value (status)
    if (status != 0xFF00 && status != 0x0000) {
        this->exceptionResponse(fn, EX_ILLEGAL_VALUE);
        return;
    }

    //Check Address and execute (reg exists?)
    if (!this->Coil(reg, (bool)status)) {
        this->exceptionResponse(fn, EX_ILLEGAL_ADDRESS);
        return;
    }

    //Check for failure
    if (this->Coil(reg) != (bool)status) {
        this->exceptionResponse(fn, EX_SLAVE_FAILURE);
        return;
    }

    _reply = REPLY_ECHO;
}

void Modbus::successResponce(uint16_t startreg, uint16_t numoutputs, FunctionCode fn) {
    //Clean frame buffer
    free(_frame);
	_len = 5;
    _frame = (uint8_t*) malloc(_len);

    _frame[0] = fn;
    _frame[1] = startreg >> 8;
    _frame[2] = startreg & 0x00FF;
    _frame[3] = numoutputs >> 8;
    _frame[4] = numoutputs & 0x00FF;
}

void Modbus::writeMultipleCoils(uint8_t* frame, uint16_t startreg, uint16_t numoutputs, uint8_t bytecount, FunctionCode fn) {
    uint8_t bitn = 0;
    uint16_t totoutputs = numoutputs;
    uint16_t i;
	while (numoutputs--) {
        i = (totoutputs - numoutputs) / 8;
        this->Coil(startreg, bitRead(frame[i], bitn));
        //increment the bit index
        bitn++;
        if (bitn == 8) bitn = 0;
        //increment the register
        startreg++;
	}

    //_reply = REPLY_NORMAL;
}

bool Modbus::onGet(uint16_t address, cbModbus cb, uint16_t numregs) {
	TRegister* reg;
	bool atLeastOne = false;
	while (numregs > 0) {
		reg = this->searchRegister(address);
		if (reg) {
			reg->get = cb;
			atLeastOne = true;
		}
		address++;
		numregs--;
	}
	return atLeastOne;
}
bool Modbus::onSet(uint16_t address, cbModbus cb, uint16_t numregs) {
	TRegister* reg;
	bool atLeastOne = false;
	while (numregs > 0) {
		reg = this->searchRegister(address);
		if (reg) {
			reg->set = cb;
			atLeastOne = true;
		}
		address++;
		numregs--;
	}
	return atLeastOne;
}

bool Modbus::readSlave(uint16_t startreg, uint16_t numregs, FunctionCode fn) {
	free(_frame);
	_len = 5;
	_frame = (uint8_t*) malloc(_len);

	_frame[0] = fn;
	_frame[1] = startreg >> 8;
	_frame[2] = startreg & 0x00FF;
	_frame[3] = numregs >> 8;
	_frame[4] = numregs & 0x00FF;
   // Serial.print("_frame[2] = ");
   //Serial.println(_frame[2]);
	return true;
}

bool Modbus::writeSlaveBits(uint16_t startreg, uint16_t numregs, FunctionCode fn) {
	free(_frame);
	_len = 5;
	_frame = (uint8_t*) malloc(_len);
	if (!_frame) return false;
	_frame[0] = fn;
	_frame[1] = startreg >> 8;
	_frame[2] = startreg & 0x00FF;
	_frame[3] = numregs >> 8;
	_frame[4] = numregs & 0x00FF;
	return true;
}

bool Modbus::writeSlaveWords(uint16_t startreg, uint16_t numregs, FunctionCode fn) {
	free(_frame);
	_len = 5;
	_frame = (uint8_t*) malloc(_len);
	if (!_frame) return false;
	_frame[0] = fn;
	_frame[1] = startreg >> 8;
	_frame[2] = startreg & 0x00FF;
	_frame[3] = numregs >> 8;
	_frame[4] = numregs & 0x00FF;
	return true;
}

void Modbus::responcePDU(uint8_t* frame) {
    uint8_t fcode  = frame[0];
    if ((fcode & 0x80) != 0) {
        Serial.print("Error: ");
        Serial.println(_frame[1]);
	    _reply = REPLY_ERROR;
	    return;
    }
    //uint16_t field1 = (uint16_t)frame[1] << 8 | (uint16_t)frame[2];
    //uint16_t field2 = (uint16_t)frame[3] << 8 | (uint16_t)frame[4];
    uint16_t field1 = 100;
    uint16_t field2 = 1;
Serial.println(fcode);
    switch (fcode) {
        case FC_READ_REGS:
            //field1 = startreg, field2 = status
            this->writeMultipleRegisters(frame, field1, field2, frame[5]);
            _reply = REPLY_OFF;
        break;
        case FC_READ_COILS:
            //field1 = startreg, field2 = numoutputs
            Serial.println(field1);
            Serial.println(field2);
            Serial.println(frame[0]);
            this->writeMultipleCoils(frame, field1, field2, frame[0]);
            _reply = REPLY_OFF;
        break;
        case FC_READ_INPUT_STAT:
            _reply = REPLY_OFF;
        break;
        case FC_READ_INPUT_REGS:
            _reply = REPLY_OFF;
        break;
        case FC_WRITE_REG:
        break;
        case FC_WRITE_REGS:
        break;
        case FC_WRITE_COIL:
        break;
        case FC_WRITE_COILS:
        break;
        default:
		_reply = REPLY_ERROR;
    }
    _reply = REPLY_OFF;
}

void Modbus::cbEnable(bool state) {
    cbEnabled = state;
}