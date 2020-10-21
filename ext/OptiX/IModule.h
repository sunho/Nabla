// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#ifndef __IRR_EXT_OPTIX_MODULE_H_INCLUDED__
#define __IRR_EXT_OPTIX_MODULE_H_INCLUDED__

#include "irr/core/core.h"

#include "optix.h"

namespace irr
{
namespace ext
{
namespace OptiX
{

class IContext;


class IModule final : public core::IReferenceCounted
{
	public:
		inline OptixModule getOptiXHandle() {return module;}

	protected:
		friend class OptiX::IContext;

		IModule(const OptixModule& _module) : module(_module) {}
		~IModule()
		{
			if (module)
				optixModuleDestroy(module);
		}

		OptixModule module;
};


}
}
}

#endif