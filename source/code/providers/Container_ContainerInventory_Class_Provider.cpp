/* @migen@ */
#include <MI.h>
#include "Container_ContainerInventory_Class_Provider.h"

#include <map>
#include <string>
#include <syslog.h>
#include <unistd.h>
#include <vector>

#include "../cjson/cJSON.h"
#include "../dockerapi/DockerRemoteApi.h"

using namespace std;

MI_BEGIN_NAMESPACE

class ContainerQuery
{
private:
	///
	/// Create the REST request to inspect a container
	///
	/// \param[in] id ID of the container to be inspected
	/// \returns Request in string format
	///
	static string restDockerInspect(string id)
	{
		return "GET /containers/" + id + "/json HTTP/1.1\r\n\r\n";
	}

	///
	/// Create the REST request to list images
	///
	/// \returns Request in string format
	///
	static string restDockerImages()
	{
		return "GET /images/json?all=0 HTTP/1.1\r\n\r\n";
	}

	///
	/// Seperate the repository, image, and image tag strings
	///
	/// \param[in] id Image ID
	/// \param[in] properties Raw string of form image:imagetag or repository/image:imagetag
	///
	/// \returns vector<string> of length 3 of form [repository, image, imagetag]
	///
	static vector<string> SetImageRepositoryImageTag(string properties)
	{
		vector<string> result(3, "");

		switch (properties.size())
		{
			default:
			{
				// Find delimiters in the string of format repository/image:imagetag
				int slashLocation = properties.find('/');
				int colonLocation = properties.find(':');

				if ((unsigned)colonLocation != string::npos)
				{
					if ((unsigned)slashLocation >= properties.size())
					{
						// image:imagetag
						result[1] = properties.substr(0, colonLocation);
					}
					else
					{
						// repository/image:imagetag
						result[0] = properties.substr(0, slashLocation);
						result[1] = properties.substr(slashLocation + 1, colonLocation - slashLocation - 1);
					}

					result[2] = properties.substr(colonLocation + 1);
					break;
				}
			}
			case 0:
			{
				// Do not crash the program
				syslog(LOG_WARNING, "Container image name (%s) is improperly formed and could not be parsed in SetRepositoryImageTag", properties.c_str());
				break;
			}
		}

		return result;
	}

	///
	/// Map the image name, repository, imagetag to ID
	///
	/// /returns Map of values to ID
	///
	static map<string, vector<string> > GenerateImageNameMap()
	{
		map<string, vector<string> > result;

		// Request images
		vector<string> request(1, restDockerImages());
		vector<cJSON*> response = getResponse(request);

		// See http://docs.docker.com/reference/api/Container_remote_api_v1.21/#list-images for example output
		if (!response.empty() && response[0])
		{
			for (int i = 0; i < cJSON_GetArraySize(response[0]); i++)
			{
				cJSON* entry = cJSON_GetArrayItem(response[0], i);

				if (entry)
				{
					cJSON* tags = cJSON_GetObjectItem(entry, "RepoTags");

					if (tags && cJSON_GetArraySize(tags))
					{
						result[string(cJSON_GetObjectItem(entry, "Id")->valuestring)] = SetImageRepositoryImageTag(string(cJSON_GetArrayItem(tags, 0)->valuestring));
					}
				}
				else
				{
					syslog(LOG_WARNING, "Attempt in GenerateImageNameMap to get element %d of image list returned null", i);
				}
			}

			// Clean up object
			cJSON_Delete(response[0]);
		}
		else
		{
			syslog(LOG_WARNING, "API call in GenerateImageNameMap to list images returned null");
		}

		return result;
	}

	///
	/// Get information from container config field
	///
	/// \param[in] instance Object representing the container
	/// \param[in] entry JSON from docker inspect
	///
	static void ObtainContainerConfig(Container_ContainerInventory_Class& instance, cJSON* entry)
	{
		cJSON* config = cJSON_GetObjectItem(entry, "Config");

		if (config)
		{
			// Hostname of container
			instance.ContainerHostname_value(cJSON_GetObjectItem(config, "Hostname")->valuestring);

			// Environment variables
			instance.EnvironmentVar_value(cJSON_Print(cJSON_GetObjectItem(config, "Env")));

			// Command
			instance.Command_value(cJSON_Print(cJSON_GetObjectItem(config, "Cmd")));

			cJSON* labels = cJSON_GetObjectItem(config, "Labels");

			// Compose group
			instance.ComposeGroup_value("");

			if (labels)
			{
				cJSON* groupName = cJSON_GetObjectItem(labels, "com.docker.compose.project");

				if (groupName)
				{
					instance.ComposeGroup_value(groupName->valuestring);
				}
			}
		}
		else
		{
			syslog(LOG_WARNING, "Attempt in ObtainContainerConfig to get container %s config information returned null", cJSON_GetObjectItem(entry, "Id")->valuestring);
		}
	}

	///
	/// Get information from container state field
	///
	/// \param[in] instance Object representing the container
	/// \param[in] entry JSON from docker inspect
	///
	static void ObtainContainerState(Container_ContainerInventory_Class& instance, cJSON* entry)
	{
		cJSON* state = cJSON_GetObjectItem(entry, "State");

		if (state)
		{
			int exitCode = cJSON_GetObjectItem(state, "ExitCode")->valueint;
			instance.ExitCode_value(exitCode);

			if (exitCode)
			{
				// Container failed
				instance.State_value("Failed");
			}
			else
			{
				if (cJSON_GetObjectItem(state, "Running")->valueint)
				{
					// Container running
					instance.State_value("Running");
				}
				else if (cJSON_GetObjectItem(state, "Paused")->valueint)
				{
					// Container paused
					instance.State_value("Paused");
				}
				else
				{
					// Container exited
					instance.State_value("Stopped");
				}
			}

			instance.StartedTime_value(cJSON_GetObjectItem(state, "StartedAt")->valuestring);
			instance.FinishedTime_value(cJSON_GetObjectItem(state, "FinishedAt")->valuestring);
		}
		else
		{
			syslog(LOG_WARNING, "Attempt in ObtainContainerState to get container %s state information returned null", cJSON_GetObjectItem(entry, "Id")->valuestring);
		}
	}

	///
	/// Get information from container host config field
	///
	/// \param[in] instance Object representing the container
	/// \param[in] entry JSON from docker inspect
	///
	static void ObtainContainerHostConfig(Container_ContainerInventory_Class& instance, cJSON* entry)
	{
		cJSON* hostConfig = cJSON_GetObjectItem(entry, "HostConfig");

		if (hostConfig)
		{
			// Links
			instance.Links_value(cJSON_Print(cJSON_GetObjectItem(hostConfig, "Links")));

			// Ports
			instance.Ports_value(cJSON_Print(cJSON_GetObjectItem(hostConfig, "PortBindings")));
		}
		else
		{
			syslog(LOG_WARNING, "Attempt in ObtainContainerHostConfig to get container %s host config information returned null", cJSON_GetObjectItem(entry, "Id")->valuestring);
		}
	}

	///
	/// Inspect a container and get the necessary data
	///
	/// \param[in] id Container ID
	/// \returns Object representing the container
	///
	static Container_ContainerInventory_Class InspectContainer(string id, map<string, vector<string> >& nameMap)
	{
		// New inventory entry
		Container_ContainerInventory_Class instance;

		// Inspect container
		vector<string> request(1, restDockerInspect(id));
		vector<cJSON*> response = getResponse(request);

		// See http://docs.docker.com/reference/api/Container_remote_api_v1.21/#inspect-a-container for example output
		if (!response.empty() && response[0])
		{
			instance.InstanceID_value(cJSON_GetObjectItem(response[0], "Id")->valuestring);
			instance.CreatedTime_value(cJSON_GetObjectItem(response[0], "Created")->valuestring);

			string imageId = string(cJSON_GetObjectItem(response[0], "Image")->valuestring);
			instance.ImageId_value(imageId.c_str());

			if (nameMap.count(imageId))
			{
				instance.Repository_value(nameMap[imageId][0].c_str());
				instance.Image_value(nameMap[imageId][1].c_str());
				instance.ImageTag_value(nameMap[imageId][2].c_str());
			}

			ObtainContainerConfig(instance, response[0]);
			ObtainContainerState(instance, response[0]);
			ObtainContainerHostConfig(instance, response[0]);

			// Clean up object
			cJSON_Delete(response[0]);
		}
		else
		{
			syslog(LOG_WARNING, "Attempt in InspectContainer to inspect %s returned null", id.c_str());
		}

		return instance;
	}

public:
	///
	/// Get information about all containers on the host
	///
	/// \returns Vector containing objects representing each container
	///
	static vector<Container_ContainerInventory_Class> QueryAll()
	{
		openlog("Container_ContainerInventory", LOG_PID | LOG_NDELAY, LOG_LOCAL1);

		// Get computer name
		char name[256];
		string hostname = gethostname(name, 256) ? "" : string(name);

		vector<Container_ContainerInventory_Class> result;

		vector<string> containerIds = listContainer(true);
		map<string, vector<string> > nameMap = GenerateImageNameMap();

		for (unsigned i = 0; i < containerIds.size(); i++)
		{
			result.push_back(InspectContainer(containerIds[i], nameMap));
			result[i].Computer_value(hostname.c_str());
		}

		closelog();
		return result;
	}
};

#ifdef _MSC_VER
#pragma region
#endif

Container_ContainerInventory_Class_Provider::Container_ContainerInventory_Class_Provider(Module* module) : m_Module(module){}

Container_ContainerInventory_Class_Provider::~Container_ContainerInventory_Class_Provider(){}

void Container_ContainerInventory_Class_Provider::Load(Context& context)
{
	context.Post(MI_RESULT_OK);
}

void Container_ContainerInventory_Class_Provider::Unload(Context& context)
{
	context.Post(MI_RESULT_OK);
}

void Container_ContainerInventory_Class_Provider::EnumerateInstances(Context& context, const String& nameSpace, const PropertySet& propertySet, bool keysOnly, const MI_Filter* filter)
{
	vector<Container_ContainerInventory_Class> queryResult = ContainerQuery::QueryAll();

	for (unsigned i = 0; i < queryResult.size(); i++)
	{
		context.Post(queryResult[i]);
	}

	context.Post(MI_RESULT_OK);
}

void Container_ContainerInventory_Class_Provider::GetInstance(Context& context, const String& nameSpace, const Container_ContainerInventory_Class& instanceName,
	const PropertySet& propertySet)
{
	context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerInventory_Class_Provider::CreateInstance(Context& context, const String& nameSpace, const Container_ContainerInventory_Class& newInstance)
{
	context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerInventory_Class_Provider::ModifyInstance(Context& context, const String& nameSpace, const Container_ContainerInventory_Class& modifiedInstance, const PropertySet& propertySet)
{
	context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerInventory_Class_Provider::DeleteInstance(Context& context, const String& nameSpace, const Container_ContainerInventory_Class& instanceName)
{
	context.Post(MI_RESULT_NOT_SUPPORTED);
}

#ifdef _MSC_VER
#pragma endregion
#endif

MI_END_NAMESPACE