#pragma once
#ifndef NUKEE_ID_H
#define NUKEE_ID_H

namespace nuke {

class ID {
public:
    long id;

	ID();
	ID(long id);

	void generate();
};
}  // namespace nuke

#endif // !NUKEE_ID_H
