<!-- markdownlint-disable MD041 -->
[![Khronos Vulkan][1]][2]

[1]: https://vulkan.lunarg.com/img/Vulkan_100px_Dec16.png "https://www.khronos.org/vulkan/"
[2]: https://www.khronos.org/vulkan/

# Architecture of the Vulkan Loader Interfaces
[![Creative Commons][3]][4]

<!-- Copyright &copy; 2015-2021 LunarG, Inc. -->

[3]: https://i.creativecommons.org/l/by-nd/4.0/88x31.png "Creative Commons License"
[4]: https://creativecommons.org/licenses/by-nd/4.0/
## Table of Contents
  * [Overview](#overview)
    * [Who Should Read This Document](#who-should-read-this-document)
    * [The Loader](#the-loader)
    * [Layers](#layers)
    * [Drivers](#drivers)
    * [VkConfig](#vkconfig)

  * [Important Vulkan Concepts](#important-vulkan-concepts)
    * [Instance Versus Device](#instance-versus-device)
    * [Dispatch Tables and Call Chains](#dispatch-tables-and-call-chains)

  * [Elevated Privilege Caveats](#elevated-privilege-caveats)

  * [Application Interface to the Loader](#application-interface-to-the-loader)
  * [Layer Interface with the Loader](#layer-interface-with-the-loader)
  * [Driver Interface with the Loader](#driver-interface-with-the-loader)

  * [Loader Policies](#loader-policies)
  * [Table of Debug Environment Variables](#table-of-debug-environment-variables)
  * [Glossary of Terms](#glossary-of-terms)

## Overview

Vulkan is a layered architecture, made up of the following elements:
  * The Vulkan Application
  * [The Vulkan Loader](#the-loader)
  * [Vulkan Layers](#layers)
  * [Drivers](#drivers)
  * [VkConfig](#vkconfig)

![High Level View of Loader](./images/high_level_loader.png)

The general concepts in this document are applicable to the loaders available
for Windows, Linux, Android, and macOS systems.


### Who Should Read This Document

While this document is primarily targeted at developers of Vulkan applications,
drivers and layers, the information contained in it could be useful to anyone
wanting a better understanding of the Vulkan runtime.


### The Loader

The application sits at the top and interfaces directly with the Vulkan
loader.
At the bottom of the stack sits the drivers.
A driver can control one or more physical devices capable of rendering Vulkan,
implement a conversion from Vulkan into a native graphics API (like
[MoltenVk](https://github.com/KhronosGroup/MoltenVK], or implement a fully
software path that can be executed on a CPU to simulate a Vulkan device (like
[SwiftShader](https://github.com/google/swiftshader) or LavaPipe).
Remember, Vulkan-capable hardware may be graphics-based, compute-based, or
both.
Between the application and the drivers, the loader can inject any number of
optional [layers](#layers) that provide special functionality.
The loader is critical to managing the proper dispatching of Vulkan
functions to the appropriate set of layers and drivers.
The Vulkan object model allows the loader to insert layers into a call-chain
so that the layers can process Vulkan functions prior to the driver being
called.

This document is intended to provide an overview of the necessary interfaces
between each of these.


#### Goals of the Loader

The loader was designed with the following goals in mind:
 1. Support one or more Vulkan-capable drivers on a user's system without them
 interfering with one another.
 2. Support Vulkan Layers which are optional modules that can be enabled by an
application, developer, or standard system settings.
 3. Keep the overall overhead of the loader to the minimum possible.


### Layers

Layers are optional components that augment the Vulkan development environment.
They can intercept, evaluate, and modify existing Vulkan functions on their
way from the application down to the drivers and back up.
Layers are implemented as libraries that can be enabled in different ways
and are loaded during CreateInstance.
Each layer can choose to hook, or intercept, Vulkan functions which in
turn can be ignored, inspected, or augmented.
Any function a layer does not hook is simply skipped for that layer and the
control flow will simply continue on to the next supporting layer or
driver.
Because of this, a layer can choose whether to intercept all known Vulkan
functions or only a subset it is interested in.

Some examples of features that layers may expose include:
 * Validating API usage
 * Tracing API calls
 * Debugging aids
 * Profiling
 * Overlay

Because layers are optional and dynamically loaded, they can be enabled
and disabled as desired.
For example, while developing and debugging an application, enabling 
certain layers can assist in making sure it properly uses the Vulkan API.
But when releasing the application, those layers are unnecessary
and thus won't be enabled, increasing the speed of the application.


### Drivers

The library that implements Vulkan, either through supporting a physical
hardware device directly, converting Vulkan commands into native graphics
commands, or simulating Vulkan through software, is considered "a driver".
The most common type of driver is still the Installable Client Driver (or ICD).
The loader is responsible for discovering available Vulkan drivers on the
system.
Given a list of available drivers, the loader can enumerate all the available
physical devices and provide this information for an application.


#### Installable Client Drivers

Vulkan allows multiple ICDs each supporting one or more devices.
Each of these devices is represented by a Vulkan `VkPhysicalDevice` object.
The loader is responsible for discovering available Vulkan ICDs via the standard
driver search on the system.


### VkConfig

VkConfig is a tool LunarG has developed to assist with modifying the Vulkan
environment on the local system.
It can be used to find layers, enable them, change layer settings, and other
useful features.
VkConfig can be found by either installing the
[Vulkan SDK](https://vulkan.lunarg.com/) or by building the source out of the
[LunarG VulkanTools GitHub Repo](https://github.com/LunarG/VulkanTools).

VkConfig generates three outputs, two of which work with the Vulkan loader and
layers.
These outputs are:
 * The Vulkan Override Layer
 * The Vulkan Layer Settings File
 * VkConfig Configuration Settings

These files are found in different locations based on your platform:

<table style="width:100%">
  <tr>
    <th>Platform</th>
    <th>Output</th>
    <th>Location</th>
  </tr>
  <tr>
    <th rowspan="3">Linux</th>
    <td>Vulkan Override Layer</td>
    <td>$USER/.local/share/vulkan/implicit_layer.d/VkLayer_override.json</td>
  </tr>
  <tr>
    <td>Vulkan Layer Settings</td>
    <td>$USER/.local/share/vulkan/settings.d/vk_layer_settings.txt</td>
  </tr>
  <tr>
    <td>VkConfig Configuration Settings</td>
    <td>$USER/.local/share/vulkan/settings.d/vk_layer_settings.txt</td>
  </tr>
  <tr>
    <th rowspan="3">Windows</th>
    <td>Vulkan Override Layer</td>
    <td>%HOME%\AppData\Local\LunarG\vkconfig\override\VkLayerOverride.json</td>
  </tr>
  <tr>
    <td>Vulkan Layer Settings</td>
    <td>(registry) HKEY_CURRENT_USER\Software\Khronos\Vulkan\Settings</td>
  </tr>
  <tr>
    <td>VkConfig Configuration Settings</td>
    <td>(registry) HKEY_CURRENT_USER\Software\LunarG\vkconfig </td>
  </tr>
</table>

The [Override Meta-Layer](./LoaderLayerInterface.md#override-meta-layer) is
an important part of how VkConfig works.
This layer, when found by the loader, forces the loading of the desired layers
that were enabled inside of VkConfig as well as disables those layers that
were intentionally disabled (including implicit layers).

The Vulkan Layer Settings file can be used to specify certain behaviors and
actions each enabled layer is expected to perform.
These settings can also be controlled by VkConfig, or they can be manually
enabled.
For details on what settings can be used, refer to the individual layers.

In the future, VkConfig may have additional interactions with the Vulkan
loader.

More details on VkConfig can be found in its
[GitHub documentation](https://github.com/LunarG/VulkanTools/blob/master/vkconfig/README.md).
<br/>
<br/>


## Important Vulkan Concepts

Vulkan has a few concepts that provide a fundamental basis for its organization.
These concepts should be understood by any one attempting to use Vulkan or
develop any of its components.


### Instance Versus Device

An important concept to understand, which is brought up repeatedly throughout this
document, is how the Vulkan API is organized.
Many objects, functions, extensions, and other behavior in Vulkan can be
separated into two groups:
 * [Instance-specific](#instance-specific)
 * [Device-specific](#device-specific)


#### Instance-Specific

A "Vulkan instance" (`VkInstance`) is a high-level construct used to provide
Vulkan system-level information and functionality.

##### Instance Objects

A few Vulkan objects associated directly with an instance are:
 * `VkInstance`
 * `VkPhysicalDevice`
 * `VkPhysicalDeviceGroup`

##### Instance Functions

An "instance function" is any Vulkan function where the first parameter is an
[instance object](#instance-objects) or no object at all.

Some Vulkan instance functions are:
 * `vkEnumerateInstanceExtensionProperties`
 * `vkEnumeratePhysicalDevices`
 * `vkCreateInstance`
 * `vkDestroyInstance`

An application can link directly to all core instance functions through the
Vulkan loader's headers.
Alternatively, an application can query function pointers using
`vkGetInstanceProcAddr`.
`vkGetInstanceProcAddr` can be used to query any instance or device entry-points
in addition to all core entry-points.

If `vkGetInstanceProcAddr` is called using a `VkInstance`, then any function
pointer returned is specific to that `VkInstance` and any additional objects
that are created from it.

##### Instance Extensions

Extensions to Vulkan are similarly associated based on what type of
functions they provide.
Because of this, extensions are broken up into instance or device extensions
where most, if not all of the functions, in the extension are of the
corresponding type.
For example, an "instance extension" is composed primarily of "instance
functions" which primarily take instance objects.
These will be discussed in more detail later.


#### Device-Specific

A Vulkan device (`VkDevice`), on the other-hand, is a logical identifier used
to associate functions with a particular Vulkan physical device
(`VkPhysicalDevice`) through a particular driver on a user's system.

##### Device Objects

A few of the Vulkan constructs associated directly with a device include:
 * `VkDevice`
 * `VkQueue`
 * `VkCommandBuffer`

##### Device Functions

A "device function" is any Vulkan function which takes any device object as its
first parameter or a child object of the device.
The vast majority of Vulkan functions are device functions.
Some Vulkan device functions are:
 * `vkQueueSubmit`
 * `vkBeginCommandBuffer`
 * `vkCreateEvent`

Vulkan devices functions may be queried using either `vkGetInstanceProcAddr` or
`vkGetDeviceProcAddr`.
If an application chooses to use `vkGetInstanceProcAddr`, each call will have
additional function calls built into the call chain, which will reduce
performance slightly.
If, instead, the application uses `vkGetDeviceProcAddr`, the call chain will be
more optimized to the specific device, but the returned function pointers will
**only** work for the device used when querying them.
Unlike `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` can only be used on
Vulkan device functions.

The best solution is to query instance extension functions using
`vkGetInstanceProcAddr`, and to query device extension functions using
`vkGetDeviceProcAddr`.
See
[Best Application Performance Setup](LoaderApplicationInterface.md#best-application-performance-setup)
section in the
[LoaderApplicationInterface.md](LoaderApplicationInterface.md) document for more
information on this.

##### Device Extensions

As with instance extensions, a device extension is a set of Vulkan device
functions extending the Vulkan language.
More information about device extensions can be found later in this document.


### Dispatch Tables and Call Chains

Vulkan uses an object model to control the scope of a particular action or
operation.
The object to be acted on is always the first parameter of a Vulkan call and is
a dispatchable object (see Vulkan specification section 2.3 Object Model).
Under the covers, the dispatchable object handle is a pointer to a structure,
which in turn, contains a pointer to a dispatch table maintained by the loader.
This dispatch table contains pointers to the Vulkan functions appropriate to
that object.

There are two types of dispatch tables the loader maintains:
 - Instance Dispatch Table
   - Created in the loader during the call to `vkCreateInstance`
 - Device Dispatch Table
   - Created in the loader during the call to `vkCreateDevice`

At that time the application and the system can each specify optional layers to
be included.
The loader will initialize the specified layers to create a call chain for each
Vulkan function and each entry of the dispatch table will point to the first
element of that chain.
Thus, the loader builds an instance call chain for each `VkInstance` that is
created and a device call chain for each `VkDevice` that is created.

When an application calls a Vulkan function, this typically will first hit a
*trampoline* function in the loader.
These *trampoline* functions are small, simple functions that jump to the
appropriate dispatch table entry for the object they are given.
Additionally, for functions in the instance call chain, the loader has an
additional function, called a *terminator*, which is called after all enabled
layers to marshall the appropriate information to all available drivers.


#### Instance Call Chain Example

For example, the diagram below represents what happens in the call chain for
`vkCreateInstance`.
After initializing the chain, the loader calls into the first layer's
`vkCreateInstance`, which will call the next layer's `vkCreateInstance 
before finally terminating in the loader again where it will call
every driver's `vkCreateInstance`.
This allows every enabled layer in the chain to set up what it needs based on
the `VkInstanceCreateInfo` structure from the application.

![Instance Call Chain](./images/loader_instance_chain.png)

This also highlights some of the complexity the loader must manage when using
instance call chains.
As shown here, the loader's *terminator* must aggregate information to and from
multiple drivers when they are present.
This implies that the loader has to be aware of any instance-level extensions
which work on a `VkInstance` to aggregate them correctly.


#### Device Call Chain Example

Device call chains are created in `vkCreateDevice` and are generally simpler
because they deal with only a single device.
This allows for the specific driver exposing this device to always be the
*terminator* of the chain.

![Loader Device Call Chain](./images/loader_device_chain_loader.png)
<br/>


## Elevated Privilege Caveats

To ensure that the system is safe from exploitation, Vulkan applications which
are run with elevated privileges are restricted from certain operations, such
as reading environment variables from unsecure locations or searching for
files in user controlled paths.
This is done to ensure that an application running with elevated privileges does
not run using components that were not installed in the proper approved
locations.

The loader uses platform-specific mechanisms (such as `secure_getenv` and its
equivalents) for querying sensitive environment variables to avoid accidentally
using untrusted results.

These behaviors also result in ignoring certain environment variables, such as:

  * `VK_ICD_FILENAMES`
  * `VK_LAYER_PATH`
  * `XDG_CONFIG_HOME` (Linux/Mac-specific)
  * `XDG_DATA_HOME` (Linux/Mac-specific)

For more information on the affected search paths, refer to 
[Layer Discovery](LoaderLayerInterface.md#layer-discovery) and
[Driver Discovery](LoaderDriverInterface.md#driver-discovery).
<br/>
<br/>


## Application Interface to the Loader

The Application interface to the Vulkan loader is now detailed in the
[LoaderApplicationInterface.md](LoaderApplicationInterface.md) document found in
the same directory as this file.
<br/>
<br/>


## Layer Interface with the Loader

The Layer interface to the Vulkan loader is detailed in the
[LoaderLayerInterface.md](LoaderLayerInterface.md) document found in the same
directory as this file.
<br/>
<br/>


## Driver Interface With the Loader

The Driver interface to the Vulkan loader is detailed in the
[LoaderDriverInterface.md](LoaderDriverInterface.md) document found in the same
directory as this file.
<br/>
<br/>


## Loader Policies

Loader policies with regards to the loader interaction with drivers and layers
 are now documented in the appropriate sections.
The intention of these sections is to clearly define expected behavior of the
loader with regards to its interactions with those components.
This could be especially useful in cases where a new or specialized loader may
be required that conforms to the behavior of the existing loader.
Because of this, the primary focus of those sections is on expected behaviors
for all relevant components to create a consistent experience across platforms.
In the long-run, this could also be used as validation requirements for any
existing Vulkan loaders.

To review the particular policy sections, please refer to one or both of the
sections listed below:
 * [Loader And Driver Policy](LoaderDriverInterface.md#loader-and-driver-policy)
 * [Loader And Layer Policy](LoaderLayerInterface.md#loader-and-layer-policy)
<br/>
<br/>


## Table of Debug Environment Variables

The following are all the Debug Environment Variables available for use with the
Loader.
These are referenced throughout the text, but collected here for ease of
discovery.

<table style="width:100%">
  <tr>
    <th>Environment Variable</th>
    <th>Behavior</th>
    <th>Example Format</th>
  </tr>
  <tr>
    <td><small><i>VK_ICD_FILENAMES</i></small></td>
    <td>Force the loader to use the specific ICD JSON files.
        The value contains a list of delimited full path listings to
        driver JSON Manifest files.<br/>
        <b>NOTE:</b> If a global path to the JSON file is not used, issues
        may be encountered.<br/>
        <b>Ignored when running Vulkan application in executing with
        elevated privileges.</b>
        See <a href="#elevated-privilege-caveats">Elevated Privilege Caveats</a>
        for more information.
    </td>
    <td><small>export<br/>
        &nbsp;&nbsp;VK_ICD_FILENAMES=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<folder_a>/intel.json:<folder_b>/amd.json
        <br/> <br/>
        set<br/>
        &nbsp;&nbsp;VK_ICD_FILENAMES=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<folder_a>\nvidia.json;<folder_b>\mesa.json
        </small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_INSTANCE_LAYERS</i></small></td>
    <td>Force the loader to add the given layers to the list of Enabled layers
        normally passed into <b>vkCreateInstance</b>.
        These layers are added first, and the loader will remove any duplicate
        layers that appear in both this list as well as that passed into
        <i>ppEnabledLayerNames</i>.
    </td>
    <td><small>export<br/>
        &nbsp;&nbsp;VK_INSTANCE_LAYERS=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&lt;layer_a&gt;;&lt;layer_b&gt;<br/><br/>
        set<br/>
        &nbsp;&nbsp;VK_INSTANCE_LAYERS=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&lt;layer_a&gt;;&lt;layer_b&gt;</small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_LAYER_PATH</i></small></td>
    <td>Override the loader's standard Layer library search folders and use the
        provided delimited folders to search for explicit layer manifest files.
        <br/>
        <b>Ignored when running Vulkan application in executing with
        elevated privileges.</b>
        See <a href="#elevated-privilege-caveats">Elevated Privilege Caveats</a>
        for more information.
    </td>
    <td><small>export<br/>
        &nbsp;&nbsp;VK_LAYER_PATH=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&lt;path_a&gt;;&lt;path_b&gt;<br/><br/>
        set<br/>
        &nbsp;&nbsp;VK_LAYER_PATH=<br/>
        &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&lt;path_a&gt;;&lt;path_b&gt;</small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_LOADER_DEVICE_SELECT</i></small></td>
    <td><b>Linux Only</b><br/>
        Allows the user to force a particular device to be prioritized above all
        other devices in the return order of <i>vkGetPhysicalDevices<i> and
        <i>vkGetPhysicalDeviceGroups<i> functions.<br/>
        The value should be "<hex vendor id>:<hex device id>".<br/>
        <b>NOTE:</b> This not remove devices.
    </td>
    <td><small>set VK_LOADER_DEVICE_SELECT=0x10de:0x1f91</small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_LOADER_DISABLE_SELECT</i></small></td>
    <td><b>Linux Only</b><br/>
        Allows the user to disable the consistent sorting algorithm run in the
        loader before returning the set of physical devices to layers.<br/>
    </td>
    <td><small>set VK_LOADER_DISABLE_SELECT=1</small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_LOADER_DISABLE_INST_EXT_FILTER</i></small></td>
    <td>Disable the filtering out of instance extensions that the loader doesn't
        know about.
        This will allow applications to enable instance extensions exposed by
        drivers but that the loader has no support for.<br/>
        <b>NOTE:</b> This may cause the loader or application to crash.</td>
    <td><small>export<br/>
        &nbsp;&nbsp;VK_LOADER_DISABLE_INST_EXT_FILTER=1<br/><br/>
        set<br/>
        &nbsp;&nbsp;VK_LOADER_DISABLE_INST_EXT_FILTER=1</small>
    </td>
  </tr>
  <tr>
    <td><small><i>VK_LOADER_DEBUG</i></small></td>
    <td>Enable loader debug messages using a comma-delimited list of level
        options.  These options are:<br/>
        &nbsp;&nbsp;* error (only errors)<br/>
        &nbsp;&nbsp;* warn (only warnings)<br/>
        &nbsp;&nbsp;* info (only info)<br/>
        &nbsp;&nbsp;* debug (only debug)<br/>
        &nbsp;&nbsp;* layer (layer-specific output)<br/>
        &nbsp;&nbsp;* driver (driver-specific output)<br/>
        &nbsp;&nbsp;* all (report out all messages)<br/><br/>
        To enable multiple options (outside of "all") like info, warning and
        error messages, set the value to "error,warn,info".
    </td>
    <td><small>export<br/>
        &nbsp;&nbsp;VK_LOADER_DEBUG=all<br/>
        <br/>
        set<br/>
        &nbsp;&nbsp;VK_LOADER_DEBUG=warn</small>
    </td>
  </tr>
</table>
<br/>
<br/>

## Glossary of Terms

<table style="width:100%">
  <tr>
    <th>Field Name</th>
    <th>Field Value</th>
  </tr>
  <tr>
    <td>Android Loader</td>
    <td>The loader designed to work primarily for the Android OS.
        This is generated from a different code base than the Khronos loader.
        But, in all important aspects, it should be functionally equivalent.
    </td>
  </tr>
  <tr>
    <td>Khronos Loader</td>
    <td>The loader released by Khronos and currently designed to work primarily
        on Windows, Linux, macOS, Stadia, and Fuchsia.
        This is generated from a different
        <a href="https://github.com/KhronosGroup/Vulkan-Loader">code base</a>
        than the Android loader.
        But in all important aspects, it should be functionally equivalent.
    </td>
  </tr>
  <tr>
    <td>Core Function</td>
    <td>A function that is already part of the Vulkan core specification and not
        an extension. <br/>
        For example, <b>vkCreateDevice()</b>.
    </td>
  </tr>
  <tr>
    <td>Device Call Chain</td>
    <td>The call chain of functions followed for device functions.
        This call chain for a device function is usually as follows: first the
        application calls into a loader trampoline, then the loader trampoline
        calls enabled layers, and the final layer calls into the driver specific
        to the device. <br/>
        See the
        <a href="#dispatch-tables-and-call-chains">Dispatch Tables and Call
        Chains</a> section for more information.
    </td>
  </tr>
  <tr>
    <td>Device Function</td>
    <td>A device function is any Vulkan function which takes a <i>VkDevice</i>,
        <i>VkQueue</i>, <i>VkCommandBuffer</i>, or any child of these, as its
        first parameter. <br/><br/>
        Some Vulkan device functions are: <br/>
        &nbsp;&nbsp;<b>vkQueueSubmit</b>, <br/>
        &nbsp;&nbsp;<b>vkBeginCommandBuffer</b>, <br/>
        &nbsp;&nbsp;<b>vkCreateEvent</b>. <br/><br/>
        See the <a href="#instance-versus-device">Instance Versus Device</a>
        section for more information.
    </td>
  </tr>
  <tr>
    <td>Discovery</td>
    <td>The process of the loader searching for driver and layer files to set up
        the internal list of Vulkan objects available.<br/>
        On <i>Windows/Linux/macOS</i>, the discovery process typically focuses on
        searching for Manifest files.<br/>
        On <i>Android</i>, the process focuses on searching for library files.
    </td>
  </tr>
  <tr>
    <td>Dispatch Table</td>
    <td>An array of function pointers (including core and possibly extension
        functions) used to step to the next entity in a call chain.
        The entity could be the loader, a layer or a driver.<br/>
        See <a href="#dispatch-tables-and-call-chains">Dispatch Tables and Call
        Chains</a> for more information.
    </td>
  </tr>
  <tr>
    <td>Driver</td>
    <td>The underlying library which provides support for the Vulkan API.
        This support can be implemented as either an ICD, API translation
        library, or pure software.<br/>
        See <a href="#drivers">Drivers</a> section for more information.
    </td>
  </tr>
  <tr>
    <td>Extension</td>
    <td>A concept of Vulkan used to expand the core Vulkan functionality.
        Extensions may be IHV-specific, platform-specific, or more broadly
        available. <br/>
        Always first query if an extension exists, and enable it during
        <b>vkCreateInstance</b> (if it is an instance extension) or during
        <b>vkCreateDevice</b> (if it is a device extension) before attempting
        to use it. <br/>
        Extensions will always have an author prefix or suffix modifier to every
        structure, enumeration entry, command entry-point, or define that is
        associated with it.
        For example, `KHR` is the prefix for Khronos authored extensions and
        will also be found on structures, enumeration entries, and commands
        associated with those extensions.
    </td>
  </tr>
  <tr>
    <td>Extension Function</td>
    <td>A function that is defined as part of an extension and not part of the
        Vulkan core specification. <br/>
        As with the extension the function is defined as part of, it will have a
        suffix modifier indicating the author of the extension.<br/>
        Some example extension suffixes include:<br/>
        &nbsp;&nbsp;<b>KHR</b>  - For Khronos authored extensions, <br/>
        &nbsp;&nbsp;<b>EXT</b>  - For multi-company authored extensions, <br/>
        &nbsp;&nbsp;<b>AMD</b>  - For AMD authored extensions, <br/>
        &nbsp;&nbsp;<b>ARM</b>  - For ARM authored extensions, <br/>
        &nbsp;&nbsp;<b>NV</b>   - For Nvidia authored extensions.<br/>
    </td>
  </tr>
  <tr>
    <td>ICD</td>
    <td>Acronym for "Installable Client Driver".
        These are drivers that are provided by IHVs to interact with the
        hardware they provide. <br/>
        These are the most common type of Vulkan drivers. <br/>
        See <a href="#installable-client-drivers">Installable Client Drivers</a> 
        section for more information.
    </td>
  </tr>
  <tr>
    <td>IHV</td>
    <td>Acronym for an "Independent Hardware Vendor".
        Typically the company that built the underlying hardware technology
        that is being used. <br/>
        A typical examples for a Graphics IHV include (but not limited to):
        AMD, ARM, Imagination, Intel, Nvidia, Qualcomm
    </td>
  </tr>
  <tr>
    <td>Instance Call Chain</td>
    <td>The call chain of functions followed for instance functions.
        This call chain for an instance function is usually as follows: first
        the application calls into a loader trampoline, then the loader
        trampoline calls enabled layers, the final layer calls a loader
        terminator, and the loader terminator calls all available
        drivers. <br/>
        See the <a href="#dispatch-tables-and-call-chains">Dispatch Tables and
        Call Chains</a> section for more information.
    </td>
  </tr>
  <tr>
    <td>Instance Function</td>
    <td>An instance function is any Vulkan function which takes as its first
        parameter either a <i>VkInstance</i> or a <i>VkPhysicalDevice</i> or
        nothing at all. <br/><br/>
        Some Vulkan instance functions are:<br/>
        &nbsp;&nbsp;<b>vkEnumerateInstanceExtensionProperties</b>, <br/>
        &nbsp;&nbsp;<b>vkEnumeratePhysicalDevices</b>, <br/>
        &nbsp;&nbsp;<b>vkCreateInstance</b>, <br/>
        &nbsp;&nbsp;<b>vkDestroyInstance</b>. <br/><br/>
        See the <a href="#instance-versus-device">Instance Versus Device</a>
        section for more information.
    </td>
  </tr>
  <tr>
    <td>Layer</td>
    <td>Layers are optional components that augment the Vulkan system.
        They can intercept, evaluate, and modify existing Vulkan functions on
        their way from the application down to the driver.<br/>
        See the <a href="#layers">Layers</a> section for more information.
    </td>
  </tr>
  <tr>
    <td>Layer Library</td>
    <td>The <b>Layer Library</b> is the group of all layers the loader is able
        to discover.
        These may include both implicit and explicit layers.
        These layers are available for use by applications unless disabled in
        some way.
        For more info, see
        <a href="LoaderLayerInterface.md#layer-layer-discovery">Layer Discovery
        </a>.
    </td>
  </tr>
  <tr>
    <td>Loader</td>
    <td>The middleware program which acts as the mediator between Vulkan
        applications, Vulkan layers, and Vulkan drivers.<br/>
        See <a href="#the-loader">The Loader</a> section for more information.
    </td>
  </tr>
  <tr>
    <td>Manifest Files</td>
    <td>Data files in JSON format used by the Khronos loader.
        These files contain specific information for either a
        <a href="LoaderLayerInterface.md#layer-manifest-file-format">Layer</a>
        or a
        <a href="LoaderDriverInterface.md#driver-manifest-file-format">Driver</a>
        and define necessary information such as where to find files and default
        settings.
    </td>
  </tr>
  <tr>
    <td>Terminator Function</td>
    <td>The last function in the instance call chain above the driver and owned
        by the loader.
        This function is required in the instance call chain because all
        instance functionality must be communicated to all drivers capable of
        receiving the call. <br/>
        See <a href="#dispatch-tables-and-call-chains">Dispatch Tables and Call
        Chains</a> for more information.
    </td>
  </tr>
  <tr>
    <td>Trampoline Function</td>
    <td>The first function in an instance or device call chain owned by the
        loader which handles the set up and proper call chain walk using the
        appropriate dispatch table.
        On device functions (in the device call chain) this function can
        actually be skipped.<br/>
        See <a href="#dispatch-tables-and-call-chains">Dispatch Tables and Call
        Chains</a> for more information.
    </td>
  </tr>
  <tr>
    <td>WSI Extension</td>
    <td>Acronym for Windowing System Integration.
        A Vulkan extension targeting a particular Windowing system and designed
        to interface between the Windowing system and Vulkan.<br/>
        See
        <a href="LoaderApplicationInterface.md#wsi-extensions">WSI Extensions</a>
        for more information.
    </td>
  </tr>
</table>
