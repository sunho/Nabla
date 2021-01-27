#ifndef __NBL_I_OPENGL_PHYSICAL_DEVICE_BASE_H_INCLUDED__
#define __NBL_I_OPENGL_PHYSICAL_DEVICE_BASE_H_INCLUDED__

#include "nbl/video/CEGLCaller.h"
#include "nbl/video/IPhysicalDevice.h"

namespace nbl { 
namespace video
{

template <typename LogicalDeviceType>
class IOpenGL_PhysicalDeviceBase : public IPhysicalDevice
{
public:
    IOpenGL_PhysicalDeviceBase(egl::CEGLCaller* _egl) : m_egl(_egl)
    {
        // OpenGL backend emulates presence of just one queue with all capabilities (graphics, compute, transfer, ... what about sparse binding?)
        SQueueFamilyProperties qprops;
        qprops.queueFlags = EQF_GRAPHICS_BIT | EQF_COMPUTE_BIT | EQF_TRANSFER_BIT;
        qprops.queueCount = 1u;
        qprops.timestampValidBits = 64u; // ??? TODO
        qprops.minImageTransferGranularity = { 1u,1u,1u }; // ??? TODO

        m_qfamProperties = core::make_refctd_dynamic_array<qfam_props_array_t>(1u, qprops);

        // TODO fill m_properties and m_features (possibly should be done in derivative classes' ctors, not sure yet)
    }

protected:
    core::smart_refctd_ptr<ILogicalDevice> createLogicalDevice_impl(const ILogicalDevice::SCreationParams& params) final override
    {
        // TODO uncomment once GL/GLES logical device has all pure virtual methods implemented
        //return core::make_smart_refctd_ptr<LogicalDeviceType>(m_egl, params);
        return nullptr;
    }

    virtual ~IOpenGL_PhysicalDeviceBase() = default;

private:
    // physical device does nothing with this,
    // just need to forward to logical device
    egl::CEGLCaller* m_egl;
};

}
}

#endif
